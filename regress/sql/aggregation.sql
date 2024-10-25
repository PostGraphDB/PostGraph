
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
 */ 

SET extra_float_digits = 0;
LOAD 'postgraph';
SET search_path TO postgraph;
set timezone TO 'GMT';

--
-- avg(), sum(), count(), & count(*)
--
CREATE GRAPH college;
USE GRAPH college;

CREATE (:students {name: 'Jack ', gpa: 3.0, age: 21, zip: 13275}) ;
CREATE (:students {name: 'Jill ', gpa: 3.5, age: 27, zip: 87962}) ;
CREATE (:students {name: 'Jim ', gpa: 3.75, age: 32, zip: 68752}) ;
CREATE (:students {name: 'Rick ', gpa: 2.5, age: 24, zip:  '75612 '}) ;
CREATE (:students {name: 'Ann ', gpa: 3.8::numeric, age: 23}) ;
CREATE (:students {name: 'Derek ', gpa: 4.0, age: 19, zip: 65218}) ;
CREATE (:students {name: 'Jessica ', gpa: 3.9::numeric, age: 20}) ;

MATCH (u) RETURN corr(u.gpa, u.age) ;
MATCH (u) RETURN covar_pop(u.gpa, u.age) ;
MATCH (u) RETURN covar_samp(u.gpa, u.age) ;
MATCH (u) RETURN regr_sxx(u.gpa, u.age) ;
MATCH (u) RETURN regr_syy(u.gpa, u.age) ;
MATCH (u) RETURN regr_sxy(u.gpa, u.age) ;
MATCH (u) RETURN regr_slope(u.gpa, u.age) ;
MATCH (u) RETURN regr_intercept(u.gpa, u.age) ;
MATCH (u) RETURN regr_avgx(u.gpa, u.age) ;
MATCH (u) RETURN regr_avgy(u.gpa, u.age) ;
MATCH (u) RETURN regr_r2(u.gpa, u.age) ;


MATCH (u) RETURN avg(u.gpa), sum(u.gpa), sum(u.gpa)/count(u.gpa), count(u.gpa), count(*);

CREATE (:students {name:  'Dave ', age: 24});
CREATE (:students {name:  'Mike ', age: 18});

MATCH (u) RETURN (u);
MATCH (u) RETURN avg(u.gpa), sum(u.gpa), sum(u.gpa) / count(u.gpa), count(u.gpa), count(*);

--
-- min() & max()
--
MATCH (u) RETURN min(u.gpa), max(u.gpa), count(u.gpa), count(*);
MATCH (u) RETURN min(u.gpa), max(u.gpa), count(u.gpa), count(*) ;
MATCH (u) RETURN min(u.name), max(u.name), count(u.name), count(*);
MATCH (u) RETURN min(u.zip), max(u.zip), count(u.zip), count(*);

--
-- stDev() & stDevP()
--
MATCH (u) RETURN stDev(u.gpa), stDevP(u.gpa);


--
-- percentileCont() & percentileDisc()
--
MATCH (u) RETURN percentileCont(u.gpa, .55), percentileDisc(u.gpa, .55), percentileCont(u.gpa, .9), percentileDisc(u.gpa, .9);
MATCH (u) RETURN percentileCont(u.gpa, .55);
MATCH (u) RETURN percentileDisc(u.gpa, .55);

--
-- collect()
--
MATCH (u) RETURN collect(u.name), collect(u.age), collect(u.gpa), collect(u.zip);
MATCH (u) RETURN collect(u.gpa), collect(u.gpa);
MATCH (u) RETURN collect(u.zip), collect(u.zip);
MATCH (u) WHERE u.name =~  'billy ' RETURN collect(u.name);

--
-- DISTINCT inside aggregate functions
--
CREATE (:students {name:  'Sven ', gpa: 3.2, age: 27, zip: 94110});

MATCH (u) RETURN (u) ;
MATCH (u) RETURN count(u.zip), count(DISTINCT u.zip);
MATCH (u) RETURN count(u.age), count(DISTINCT u.age);

-- test AUTO GROUP BY for aggregate functions
CREATE GRAPH group_by;
USE GRAPH group_by;

CREATE (:row {i: 1, j: 2, k:3});
CREATE (:row {i: 1, j: 2, k:4});
CREATE (:row {i: 1, j: 3, k:5});
CREATE (:row {i: 2, j: 3, k:6});

MATCH (u:row) RETURN u.i, u.j, u.k;
MATCH (u:row) RETURN u.i, u.j, sum(u.k);

CREATE (:L {a: 1, b: 2, c:3});
CREATE (:L {a: 2, b: 3, c:1});
CREATE (:L {a: 3, b: 1, c:2});

/*
 * TODO: Get the link from the opencypher website.
 */
MATCH (x:L) RETURN x.a, x.b, x.c, x.a + count(*) + x.b + count(*) + x.c;
MATCH (x:L) RETURN x.a + x.b + x.c, x.a + x.b + x.c + count(*) + count(*)
;

-- with WITH clause
MATCH (x:L) WITH x, count(x) AS cnt RETURN x.a + x.b + x.c + cnt;
MATCH (x:L) WITH x, count(x) AS cnt RETURN x.a + x.b + x.c + cnt + cnt;
MATCH(x:L) WITH x.a + x.b + x.c AS v, count(x) as cnt RETURN v + cnt + cnt;
MATCH (x:L) RETURN x.a, x.a + count(*) + x.b + count(*) + x.c GROUP BY x.a, x.b, x.c;


-- Why we need explicit grouping
MATCH (x:L) RETURN x.a, x.a + count(*) + x.b + count(*) + x.c;
MATCH (x:L) RETURN x.a + count(*) + x.b + count(*) + x.c;

--
-- Explicit GROUP BY
--
MATCH (x:L) RETURN x.a + count(*) + x.b + count(*) + x.c GROUP BY x.a, x.b, x.c;
MATCH (x:L) RETURN x.a + x.b + x.c + count(*) + count(*) GROUP BY x.a + x.b + x.c;

MATCH (x:L) RETURN x.a + x.b + x.c + count(*) + count(*);
MATCH (x:L) WITH x.a + count(*) + x.b + count(*) + x.c as cnt GROUP BY x.a, x.b, x.c RETURN cnt ;
MATCH (x:L) WITH x.a + x.b + x.c + count(*) + count(*) as cnt GROUP BY x.a + x.b + x.c RETURN cnt;

--
-- Grouping Keywords
--
MATCH (x) RETURN x.i, x.j, x.k, COUNT(*) GROUP BY ROLLUP (x.i, x.j, x.k);
MATCH (x) RETURN x.i, x.j, x.k, COUNT(*) GROUP BY CUBE (x.i, x.j, x.k);

--
-- HAVING Clause
--
MATCH (x) WITH count(x.i) AS cnt GROUP BY x.i RETURN cnt;
MATCH (x) WITH count(x.i) AS cnt GROUP BY x.i HAVING count(x.i) > 1 RETURN cnt;

--
-- Window Functions
--
MATCH (x) WITH x, row_number() OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num RETURN x, row_num;
MATCH (x) WITH x, rank() OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num RETURN x, row_num;
MATCH (x) WITH x, dense_rank() OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num RETURN x, row_num;
MATCH (x) WITH x, percent_rank() OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num RETURN x, row_num;
MATCH (x) WITH x, cume_dist() OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num RETURN x, row_num;
MATCH (x) WITH x, lag(x.k) OVER (PARTITION BY COALESCE(x.i, x.a)  ORDER BY id(x) ) AS row_num RETURN x, row_num;


MATCH (x)
WITH x, lag(x.k) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, lead(x.k) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, first_value(x.k) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, nth_value(x.k, 2) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, ntile(1) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, ntile(2) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, ntile(3) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, last_value(x.k) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, row_number() OVER w AS row_num WINDOW w AS (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) )
RETURN x, row_num;

MATCH (x)
WITH x, row_number() OVER w AS row_num WINDOW w AS (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) 
RETURN x, row_num;

MATCH (x)
WITH x, 
    row_number() OVER (
        PARTITION BY x.i
        ORDER BY COALESCE(x.j, x.c)
        ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
    ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, 
    row_number() OVER (
        PARTITION BY x.i  
        ORDER BY COALESCE(x.j, x.c)
        ROWS BETWEEN 1 PRECEDING AND UNBOUNDED FOLLOWING
    ) AS row_num
RETURN x, row_num;

MATCH (x)
WITH x, 
    row_number() OVER (
        PARTITION BY x.i  
        ORDER BY COALESCE(x.j, x.c)
        ROWS BETWEEN UNBOUNDED PRECEDING AND 1 FOLLOWING
    ) AS row_num
RETURN x, row_num;



MATCH (x) RETURN x, last_value(x.k) OVER (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) ) AS row_num;
MATCH (x) RETURN x, row_number() OVER w AS row_num WINDOW w AS (PARTITION BY x.i ORDER BY COALESCE(x.j, x.c) );

--
-- FILTER Clause
--
MATCH (x) RETURN count(*) FILTER (WHERE x.i IS NOT NULL);

--
-- Within Group
--
MATCH (x) RETURN x.i, rank(x.i, x.j) WITHIN GROUP (ORDER BY x.k) GROUP BY x.i;


--
-- Aggregate Functions for Edge Type
--
CREATE GRAPH edge_aggregates;
USE GRAPH edge_aggregates;

CREATE ()-[:e]->();
CREATE ()-[:e]->();
CREATE ()-[:e]->();

-- Within Group

MATCH ()-[e]->() RETURN collect(e);
MATCH ()-[e]->() RETURN collect(e, 2);

MATCH () RETURN COUNT(*);

--
-- Cleanup
--
DROP GRAPH college CASCADE;
DROP GRAPH edge_aggregates CASCADE;
DROP GRAPH group_by CASCADE;

--
-- End of tests
--
