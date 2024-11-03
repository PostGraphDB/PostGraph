/*
 * PostGraph
 * Copyright (C) 2023 PostGraphDB
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
SET extra_float_digits = 0;
LOAD 'postgraph';
LOAD 'postgis-3';
SET search_path TO postgraph, public;
set timezone TO 'GMT';
CREATE GRAPH postgis;
USE GRAPH postgis;
RETURN 'box(1 2, 5 6)'::box2d;
RETURN 'BOX3D(1 2 3, 4 5 6)'::box3d;
RETURN 'SPHEROID["WGS 84",6378137,298.257223563]'::spheroid;


RETURN 'POLYHEDRALSURFACE Z (
  ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)),
  ((0 0 0, 0 1 0, 0 1 1, 0 0 1, 0 0 0)),
  ((0 0 0, 1 0 0, 1 0 1, 0 0 1, 0 0 0)),
  ((1 1 1, 1 0 1, 0 0 1, 0 1 1, 1 1 1)),
  ((1 1 1, 1 0 1, 1 0 0, 1 1 0, 1 1 1)),
  ((1 1 1, 1 1 0, 0 1 0, 0 1 1, 1 1 1))
)'::geometry;

--
-- Box3D
--
WITH u AS (SELECT 'BOX3D(1 2 3, 4 5 6)'::box3d AS g)
SELECT ST_XMin((g)), ST_YMin(g), ST_ZMin(g), ST_XMax(g), ST_YMax(g), ST_ZMax(g)from u;
WITH 'BOX3D(1 2 3, 4 5 6)'::box3d AS g	
RETURN ST_XMin((g)), ST_YMin(g), ST_ZMin(g), ST_XMax(g), ST_YMax(g), ST_ZMax(g);
--
-- Geometry
--

--
-- I/O Routines
--
-- NOTE: The first two queries for each type is the PostGIS Geometry type, used to validate I/O is exactly the same
--

-- Point (2 Dimensional) I/O
SELECT 'POINT( 1 2 )'::geometry;
SELECT ST_AsEWKT('POINT( 1 2 )'::geometry);
RETURN 'POINT( 1 2 )'::geometry;
RETURN ST_AsEWKT('POINT( 1 2 )'::geometry) ;
RETURN ST_AsEWKT('POINT( 1 2 )') ;
-- Point (3 Dimensional) I/O
SELECT 'POINT( 1 2 3 )'::geometry;
SELECT ST_AsEWKT('POINT( 1 2 3)'::geometry);
RETURN 'POINT( 1 2 3 )'::geometry;
RETURN ST_AsEWKT('POINT( 1 2 3 )'::geometry) ;
RETURN ST_AsEWKT('POINT( 1 2 3 )') ;
-- LineString (2D, 4 Points) I/O
SELECT 'LINESTRING (0 0, 1 1, 2 2, 3 3 , 4 4)'::geometry;
SELECT ST_AsEWKT('LINESTRING (0 0, 1 1, 2 2, 3 3 , 4 4)'::geometry);
RETURN 'LINESTRING (0 0, 1 1, 2 2, 3 3 , 4 4)'::geometry;
RETURN ST_AsEWKT('LINESTRING (0 0, 1 1, 2 2, 3 3 , 4 4)'::geometry) ;
RETURN ST_AsEWKT('LINESTRING (0 0, 1 1, 2 2, 3 3 , 4 4)') ;
-- LineString (3D, 4 Points) I/O
SELECT 'LINESTRING (0 0 0, 1 1 1, 2 2 2, 3 3 3, 4 4 4)'::geometry;
SELECT ST_AsEWKT('LINESTRING (0 0 0, 1 1 1, 2 2 2, 3 3 3 , 4 4 4)'::geometry);
RETURN 'LINESTRING (0 0 0, 1 1 1, 2 2 2, 3 3 3 , 4 4 4)'::geometry;
RETURN ST_AsEWKT('LINESTRING (0 0 0, 1 1 1, 2 2 2, 3 3 3, 4 4 4)'::geometry) ;
RETURN ST_AsEWKT('LINESTRING (0 0 0, 1 1 1, 2 2 2, 3 3 3 , 4 4 4)') ;
-- LineString (3D, 2 Points) I/O
SELECT 'LINESTRING (1 2 3, 10 20 30)'::geometry;
SELECT ST_AsEWKT('LINESTRING (1 2 3, 10 20 30)'::geometry);
RETURN 'LINESTRING (1 2 3, 10 20 30)'::geometry;
RETURN ST_AsEWKT('LINESTRING (1 2 3, 10 20 30)'::geometry) ;
RETURN ST_AsEWKT('LINESTRING (1 2 3, 10 20 30)') ;
-- Polygon (2D) I/O
SELECT 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry;
SELECT ST_AsEWKT('POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry);
RETURN 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry;
RETURN ST_AsEWKT('POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry) ;
RETURN ST_AsEWKT('POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )') ;
-- Polygon (3D) I/O
SELECT 'POLYGON( (0 0 1 , 10 0 1, 10 10 1, 0 10 1, 0 0 1) )'::geometry;
SELECT ST_AsEWKT('POLYGON( (0 0 1 , 10 0 1, 10 10 1, 0 10 1, 0 0 1) )'::geometry);
RETURN 'POLYGON( (0 0 1 , 10 0 1, 10 10 1, 0 10 1, 0 0 1) )'::geometry;
RETURN ST_AsEWKT('POLYGON( (0 0 1 , 10 0 1, 10 10 1, 0 10 1, 0 0 1) )'::geometry) ;
RETURN ST_AsEWKT('POLYGON( (0 0 1 , 10 0 1, 10 10 1, 0 10 1, 0 0 1) )') ;
--
-- Constructors
--

--
-- ST_MakePoint
--
SELECT ST_MakePoint(1, 0);
SELECT ST_MakePoint(1, 0, 2);
SELECT ST_MakePoint(1, 0, 2, 3);
 RETURN ST_MakePoint(1, 0) ;
 RETURN ST_MakePoint(1, 0, 2) ;
 RETURN ST_MakePoint(1, 0, 2, 3) ;
--
-- ST_MakePointM
--
SELECT ST_MakePointM(1, 0, 2);
 RETURN ST_MakePointM(1, 0, 2) ;
--
-- addbbox
--
SELECT ST_AsEwkt(postgis_addbbox('SRID=4326;POINT(1e+15 1e+15)'::geometry));
RETURN ST_AsEwkt(postgis_addbbox('SRID=4326;POINT(1e+15 1e+15)'::geometry)) ;

--
-- Comparison Operators
--
WITH p0 AS (SELECT 'POINT(0 0)'::geometry::gtype geom),
p1 AS (SELECT 'POINT(1 1)'::geometry::gtype geom)
SELECT '#4445',
  p0.geom  <    p0.geom,
  p0.geom  <=   p0.geom,
  p0.geom  =    p0.geom,
  p0.geom  >=   p0.geom,
  p0.geom  >    p0.geom,
  p0.geom  <    p1.geom,
  p0.geom  <=   p1.geom,
  p0.geom  =    p1.geom,
  p0.geom  >=   p1.geom,
  p0.geom  >    p1.geom,
  p1.geom  <    p0.geom,
  p1.geom  <=   p0.geom,
  p1.geom  =    p0.geom,
  p1.geom  >=   p0.geom,
  p1.geom  >    p0.geom
FROM p0, p1;


--
-- 2D Operators
--
-- ~=
select 'POINT(1 1)'::geometry ~= 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry ~= 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry ~= 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry ~= 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry ~= 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry ~= 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry ~= 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry ~= 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry ~= 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry ~= 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry ~= 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry ~= 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry ~= 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry ~= 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry ~= 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry ~= 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry ~= 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry ~= 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry ~= 'LINESTRING(1 1, 2 2, 3 3)'::geometry;
-- @
select 'POINT(1 1)'::geometry @ 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry @ 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry @ 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry @ 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry @ 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry @ 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry @ 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry @ 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry @ 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry @ 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry @ 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry @ 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry @ 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry @ 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry @ 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry @ 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry @ 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry @ 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry @ 'LINESTRING(1 1, 2 2, 3 3)'::geometry;
-- &<
select 'POINT(1 1)'::geometry &< 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &< 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &< 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry &< 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry &< 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &< 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry &< 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &< 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &< 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry &< 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &< 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &< 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry &< 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry &< 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &< 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &< 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry &< 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &< 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &< 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

--<<|
select 'POINT(1 1)'::geometry <<| 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry <<| 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry <<| 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry <<| 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry <<| 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry <<| 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry <<| 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry <<| 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry <<| 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry <<| 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry <<| 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry <<| 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry <<| 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry <<| 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry <<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry <<| 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry <<| 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry <<| 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry <<| 'LINESTRING(1 1, 2 2, 3 3)'::geometry;
-- &<|
select 'POINT(1 1)'::geometry &<| 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &<| 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &<| 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry &<| 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry &<| 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &<| 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry &<| 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &<| 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &<| 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry &<| 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &<| 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &<| 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry &<| 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry &<| 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &<| 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &<| 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry &<| 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &<| 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &<| 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

-- &>
select 'POINT(1 1)'::geometry &> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry &> 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry &> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry &> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry &> 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &> 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry &> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry &> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry &> 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry &> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry &> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry &> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry &> 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &> 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry &> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

-- |&>
select 'POINT(1 1)'::geometry |&> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry |&> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry |&> 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry |&> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry |&> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |&> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry |&> 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry |&> 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry |&> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry |&> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry |&> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry |&> 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry |&> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry |&> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |&> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |&> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry |&> 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry |&> 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry |&> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;
--
-- |>>
--
select 'POINT(1 1)'::geometry |>> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry |>> 'POINT(1 1)'::geometry;
select 'POINT(1 1 0)'::geometry |>> 'POINT(1 1 0)'::geometry;
select 'MULTIPOINT(1 1,2 2)'::geometry |>> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'MULTIPOINT(2 2, 1 1)'::geometry |>> 'MULTIPOINT(1 1,2 2)'::geometry;
select 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(4 5 6, 1 2 3)'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
select 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |>> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
select 'LINESTRING(1 1,2 2)'::geometry |>> 'POINT(1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry |>> 'LINESTRING(2 2, 1 1)'::geometry;
select 'LINESTRING(1 1, 2 2)'::geometry |>> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;

RETURN 'POINT(1 1)'::geometry |>> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry |>> 'POINT(1 1)'::geometry;
RETURN 'POINT(1 1 0)'::geometry |>> 'POINT(1 1 0)'::geometry;
RETURN 'MULTIPOINT(1 1,2 2)'::geometry |>> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'MULTIPOINT(2 2, 1 1)'::geometry |>> 'MULTIPOINT(1 1,2 2)'::geometry;
RETURN 'GEOMETRYCOLLECTION(POINT( 1 2 3),POINT(4 5 6))'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(4 5 6, 1 2 3)'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |>> 'GEOMETRYCOLLECTION(POINT( 4 5 6),POINT(1 2 3))'::geometry;
RETURN 'MULTIPOINT(1 2 3, 4 5 6)'::geometry |>> 'GEOMETRYCOLLECTION(MULTIPOINT(1 2 3, 4 5 6))'::geometry;
RETURN 'LINESTRING(1 1,2 2)'::geometry |>> 'POINT(1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry |>> 'LINESTRING(2 2, 1 1)'::geometry;
RETURN 'LINESTRING(1 1, 2 2)'::geometry |>> 'LINESTRING(1 1, 2 2, 3 3)'::geometry;
--
-- N-D Operators
--
-- nd overlap &&&


RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(3 3 3 5)'::geometry;
RETURN  'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(3 3 5 3)'::geometry;
RETURN  'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(3 5 3 3)'::geometry;
RETURN  'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(5 3 3 3)'::geometry;
RETURN  'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(3 3 3 3)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(2 4 2 4)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry &&& 'POINT(4 2 4 2)'::geometry;

-- nd contains ~~

RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(3 3 3 5)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(3 3 5 3)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(3 5 3 3)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(5 3 3 3)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(3 3 3 3)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(2 4 2 4)'::geometry;
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~ 'POINT(4 2 4 2)'::geometry;
-- nd within @@

RETURN 'POINT(3 3 3 5)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(3 3 5 3)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(3 5 3 3)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(5 3 3 3)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(3 3 3 3)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(2 4 2 4)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
RETURN 'POINT(4 2 4 2)'::geometry @@ 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry;
-- nd same ~~=
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'POINT(3 3 3 3)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(2 2 2 2, 4 4 4 5)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(2 2 2 2, 4 4 5 4)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(2 2 2 2, 4 5 4 4)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(2 2 2 2, 5 4 4 4)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(4 4 4 4, 2 2 2 2)'::geometry;
 
RETURN 'LINESTRING(2 2 2 2, 4 4 4 4)'::geometry ~~= 'LINESTRING(2 2 2 4, 2 2 4 2, 2 4 2 2, 4 2 2 2)'::geometry;
--
-- Point Cordinates
--

--
-- ST_X
--
SELECT ST_X('POINT (0 0)'::geometry);
RETURN ST_X('POINT (0 0)'::geometry) ;
RETURN X('POINT (0 0)'::geometry) ;
SELECT ST_X('POINTZ (1 2 3)'::geometry);
RETURN ST_X('POINTZ (1 2 3)'::geometry) ;
RETURN X('POINTZ (1 2 3)'::geometry) ;
SELECT ST_X('POINTM (6 7 8)'::geometry);
RETURN ST_X('POINTM (6 7 8)'::geometry) ;
RETURN X('POINTM (6 7 8)'::geometry) ;
SELECT ST_X('POINTZM (10 11 12 13)'::geometry);
RETURN ST_X('POINTZM (10 11 12 13)'::geometry) ;
RETURN X('POINTZM (10 11 12 13)'::geometry) ;
SELECT ST_X('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_X('MULTIPOINT ((0 0), (1 1))'::geometry) ;
RETURN X('MULTIPOINT ((0 0), (1 1))'::geometry) ;
SELECT ST_X('LINESTRING (0 0, 1 1)'::geometry);
RETURN ST_X('LINESTRING (0 0, 1 1)'::geometry) ;
RETURN X('LINESTRING (0 0, 1 1)'::geometry) ;
SELECT ST_X('GEOMETRYCOLLECTION (POINT(0 0))'::geometry);
RETURN ST_X('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
RETURN X('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
SELECT ST_X('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry);
RETURN ST_X('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
RETURN X('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
--
-- ST_Y
--
SELECT ST_Y('POINT (0 0)'::geometry);
RETURN ST_Y('POINT (0 0)'::geometry) ;
RETURN Y('POINT (0 0)'::geometry) ;
SELECT ST_Y('POINTZ (1 2 3)'::geometry);
RETURN ST_Y('POINTZ (1 2 3)'::geometry) ;
RETURN Y('POINTZ (1 2 3)'::geometry) ;
SELECT ST_Y('POINTM (6 7 8)'::geometry);
RETURN ST_Y('POINTM (6 7 8)'::geometry) ;
RETURN Y('POINTM (6 7 8)'::geometry) ;
SELECT ST_Y('POINTZM (10 11 12 13)'::geometry);
RETURN ST_Y('POINTZM (10 11 12 13)'::geometry) ;
RETURN Y('POINTZM (10 11 12 13)'::geometry) ;
SELECT ST_Y('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_Y('MULTIPOINT ((0 0), (1 1))'::geometry) ;
RETURN Y('MULTIPOINT ((0 0), (1 1))'::geometry) ;
SELECT ST_Y('LINESTRING (0 0, 1 1)'::geometry);
RETURN ST_Y('LINESTRING (0 0, 1 1)'::geometry) ;
RETURN Y('LINESTRING (0 0, 1 1)'::geometry) ;
SELECT ST_Y('GEOMETRYCOLLECTION (POINT(0 0))'::geometry);
RETURN ST_Y('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
RETURN Y('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
SELECT ST_Y('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry);
RETURN ST_Y('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
RETURN Y('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
--
-- ST_Z
--
SELECT ST_Z('POINT (0 0)'::geometry);
RETURN ST_Z('POINT (0 0)'::geometry) ;
RETURN Z('POINT (0 0)'::geometry) ;
SELECT ST_Z('POINTZ (1 2 3)'::geometry);
RETURN ST_Z('POINTZ (1 2 3)'::geometry) ;
RETURN Z('POINTZ (1 2 3)'::geometry) ;
SELECT ST_Z('POINTM (6 7 8)'::geometry);
RETURN ST_Z('POINTM (6 7 8)'::geometry) ;
RETURN Z('POINTM (6 7 8)'::geometry) ;
SELECT ST_Z('POINTZM (10 11 12 13)'::geometry);
RETURN ST_Z('POINTZM (10 11 12 13)'::geometry) ;
RETURN Z('POINTZM (10 11 12 13)'::geometry) ;
SELECT ST_Z('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_Z('MULTIPOINT ((0 0), (1 1))'::geometry) ;
RETURN Z('MULTIPOINT ((0 0), (1 1))'::geometry) ;
SELECT ST_Z('LINESTRING (0 0, 1 1)'::geometry);
RETURN ST_Z('LINESTRING (0 0, 1 1)'::geometry) ;
RETURN Z('LINESTRING (0 0, 1 1)'::geometry) ;
SELECT ST_Z('GEOMETRYCOLLECTION (POINT(0 0))'::geometry);
RETURN ST_Z('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
RETURN Z('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
SELECT ST_Z('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry);
RETURN ST_Z('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
RETURN Z('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
--
-- ST_M
--
SELECT ST_M('POINT (0 0)'::geometry);
RETURN ST_M('POINT (0 0)'::geometry) ;
RETURN M('POINT (0 0)'::geometry) ;
SELECT ST_M('POINTZ (1 2 3)'::geometry);
RETURN ST_M('POINTZ (1 2 3)'::geometry) ;
RETURN M('POINTZ (1 2 3)'::geometry) ;
SELECT ST_M('POINTM (6 7 8)'::geometry);
RETURN ST_M('POINTM (6 7 8)'::geometry) ;
RETURN M('POINTM (6 7 8)'::geometry) ;
SELECT ST_M('POINTZM (10 11 12 13)'::geometry);
RETURN ST_M('POINTZM (10 11 12 13)'::geometry) ;
RETURN M('POINTZM (10 11 12 13)'::geometry) ;
SELECT ST_M('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_M('MULTIPOINT ((0 0), (1 1))'::geometry) ;
RETURN M('MULTIPOINT ((0 0), (1 1))'::geometry) ;
SELECT ST_M('LINESTRING (0 0, 1 1)'::geometry);
RETURN ST_M('LINESTRING (0 0, 1 1)'::geometry) ;
RETURN M('LINESTRING (0 0, 1 1)'::geometry) ;
SELECT ST_M('GEOMETRYCOLLECTION (POINT(0 0))'::geometry);
RETURN ST_M('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
RETURN M('GEOMETRYCOLLECTION (POINT(0 0))'::geometry) ;
SELECT ST_M('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry);
RETURN ST_M('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
RETURN M('GEOMETRYCOLLECTION (POINT(0 1), LINESTRING(0 0, 1 1))'::geometry) ;
--
-- Affines
--
-- ST_Scale
select ST_asewkt(ST_Scale('POINT(1 1)'::geometry, 5, 5));
select ST_asewkt(ST_Scale('POINT(1 1)'::geometry, 3, 2));
select ST_asewkt(ST_Scale('POINT(10 20 -5)'::geometry, 4, 2, -8));
select ST_asewkt(ST_Scale('POINT(10 20 -5 3)'::geometry, ST_MakePoint(4, 2, -8)));
select ST_asewkt(ST_Scale('POINT(-2 -1 3 2)'::geometry, ST_MakePointM(-2, 3, 4)));
select ST_asewkt(ST_Scale('POINT(10 20 -5 3)'::geometry, ST_MakePoint(-3, 2, -1, 3)));
select st_astext(st_scale('LINESTRING(1 1, 2 2)'::geometry, 'POINT(2 2)'::geometry, 'POINT(1 1)'::geometry));

RETURN ST_asewkt(ST_Scale('POINT(1 1)'::geometry, 5, 5));
RETURN ST_asewkt(ST_Scale('POINT(1 1)'::geometry, 3, 2));
RETURN ST_asewkt(ST_Scale('POINT(10 20 -5)'::geometry, 4, 2, -8));
RETURN ST_asewkt(ST_Scale('POINT(10 20 -5 3)'::geometry, ST_MakePoint(4, 2, -8)));
RETURN ST_asewkt(ST_Scale('POINT(-2 -1 3 2)'::geometry, ST_MakePointM(-2, 3, 4)));
RETURN ST_asewkt(ST_Scale('POINT(10 20 -5 3)'::geometry, ST_MakePoint(-3, 2, -1, 3)));
RETURN ST_asewkt(st_scale('LINESTRING(1 1, 2 2)'::geometry, 'POINT(2 2)'::geometry, 'POINT(1 1)'::geometry));



RETURN '200', ST_Expand(null::geometry, 1);
RETURN '201', ST_AsText(ST_Expand('LINESTRING (1 2 3, 10 20 30)'::geometry, 1));
RETURN '202', ST_AsText(ST_Expand('LINESTRINGM (1 2 3, 10 20 30)'::geometry, 1));
RETURN '203', ST_AsText(ST_Expand('LINESTRING (1 2, 10 20)'::geometry, 3));
RETURN '204', ST_AsText(ST_Expand('POLYGON EMPTY'::geometry, 4));
RETURN '205', ST_AsText(ST_Expand('POINT EMPTY'::geometry, 2));
RETURN '206', ST_AsText(ST_Expand('POINT (2 3)'::geometry, 0));
RETURN '207', ST_AsText(ST_Expand('LINESTRING (1 2, 3 4)'::geometry, 0));
RETURN '208', ST_AsText(ST_Expand('POINT (0 0)'::geometry, -1));
RETURN '209', ST_AsText(ST_Expand('LINESTRING (0 0, 10 10)'::geometry, -4));
RETURN '210', ST_Expand(null::box3d, 1);
RETURN '211', ST_Expand('BOX3D(-1 3 5, -1 6 8)'::BOX3D, 1);
RETURN '212', ST_Expand(null::box2d, 1);
RETURN '213', ST_Expand('BOX(-2 3, -1 6'::BOX2D, 4);

RETURN '214', ST_Expand(null::geometry, 1, 1, 1, 1);
RETURN '215', ST_AsText(ST_Expand('LINESTRING (1 2 3, 10 20 30)'::geometry, 1, 4, 2, 7));

RETURN '216', ST_AsText(ST_Expand('LINESTRINGM (1 2 3, 10 20 30)'::geometry, 1, 4, 2, 7));
RETURN '217', ST_AsText(ST_Expand('LINESTRING (1 2, 10 20)'::geometry, 1, 4, 2, 7));
RETURN '218', ST_AsText(ST_Expand('POLYGON EMPTY'::geometry, 4, 3, 1, 1));
RETURN '219', ST_AsText(ST_Expand('POINT EMPTY'::geometry, 2, 3, 1, -1));
RETURN '220', ST_AsText(ST_Expand('POINT (2 3)'::geometry, 0, 4, -2, 8));
RETURN '221', ST_AsText(ST_Expand('POINT (0 0)'::geometry, -1, -2));
RETURN '222', ST_Expand(null::box3d, 1, 1, 1);
RETURN '223', ST_Expand('BOX3D(-1 3 5, -1 6 8)'::BOX3D, 1, -1, 7);
RETURN '224', ST_Expand(null::box2d, 1, 1);
RETURN '225', ST_Expand('BOX(-2 3, -1 6'::BOX2D, 4, 2);
RETURN '226', ST_SRID(ST_Expand('SRID=4326;POINT (0 0)'::geometry, 1))=4326;


--
-- Measures
--


--
-- ST_IsPolygonCW
--
-- Non-applicable types
SELECT ST_IsPolygonCW('POINT (0 0)'::geometry);
SELECT ST_IsPolygonCW('MULTIPOINT ((0 0), (1 1))'::geometry);
SELECT ST_IsPolygonCW('LINESTRING (1 1, 2 2)'::geometry);
SELECT ST_IsPolygonCW('MULTILINESTRING ((1 1, 2 2), (3 3, 0 0))'::geometry);
-- EMPTY handling
SELECT ST_IsPolygonCW('POLYGON EMPTY'::geometry);
-- Single polygon, ccw exterior ring only
SELECT ST_IsPolygonCW('POLYGON ((0 0, 1 0, 1 1, 0 0))'::geometry);
-- Single polygon, cw exterior ring only
SELECT ST_IsPolygonCW('POLYGON ((0 0, 1 1, 1 0, 0 0))'::geometry);
-- Single polygon, ccw exterior ring, cw interior rings
SELECT ST_IsPolygonCW('POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, ccw interior rings
SELECT ST_IsPolygonCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, ccw exerior ring, mixed interior rings
SELECT ST_IsPolygonCW( 'POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, mixed interior rings
SELECT ST_IsPolygonCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- MultiPolygon, ccw exterior rings only
SELECT ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 0, 101 1, 100 0)))'::geometry);
-- MultiPolygon, cw exterior rings only
SELECT ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 1, 1 0, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
-- MultiPolygon, mixed exterior rings
SELECT ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);

RETURN ST_IsPolygonCW('POINT (0 0)'::geometry);
RETURN ST_IsPolygonCW('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_IsPolygonCW('LINESTRING (1 1, 2 2)'::geometry);
RETURN ST_IsPolygonCW('MULTILINESTRING ((1 1, 2 2), (3 3, 0 0))'::geometry);
-- EMPTY handling

RETURN ST_IsPolygonCW('POLYGON EMPTY'::geometry);
-- Single polygon, ccw exterior ring only

RETURN ST_IsPolygonCW('POLYGON ((0 0, 1 0, 1 1, 0 0))'::geometry);
-- Single polygon, cw exterior ring only

RETURN ST_IsPolygonCW('POLYGON ((0 0, 1 1, 1 0, 0 0))'::geometry);
-- Single polygon, ccw exterior ring, cw interior rings

RETURN ST_IsPolygonCW('POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, ccw interior rings

RETURN ST_IsPolygonCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, ccw exerior ring, mixed interior rings

RETURN ST_IsPolygonCW( 'POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, mixed interior rings

RETURN ST_IsPolygonCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- MultiPolygon, ccw exterior rings only

RETURN ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 0, 101 1, 100 0)))'::geometry);
-- MultiPolygon, cw exterior rings only

RETURN ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 1, 1 0, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
-- MultiPolygon, mixed exterior rings

RETURN ST_IsPolygonCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
--
-- ST_IsPolygonCCW
--
-- Non-applicable types
SELECT ST_IsPolygonCCW('POINT (0 0)'::geometry);
SELECT ST_IsPolygonCCW('MULTIPOINT ((0 0), (1 1))'::geometry);
SELECT ST_IsPolygonCCW('LINESTRING (1 1, 2 2)'::geometry);
SELECT ST_IsPolygonCCW('MULTILINESTRING ((1 1, 2 2), (3 3, 0 0))'::geometry);
-- EMPTY handling
SELECT ST_IsPolygonCCW('POLYGON EMPTY'::geometry);
-- Single polygon, ccw exterior ring only
SELECT ST_IsPolygonCCW('POLYGON ((0 0, 1 0, 1 1, 0 0))'::geometry);
-- Single polygon, cw exterior ring only
SELECT ST_IsPolygonCCW('POLYGON ((0 0, 1 1, 1 0, 0 0))'::geometry);
-- Single polygon, ccw exterior ring, cw interior rings
SELECT ST_IsPolygonCCW('POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, ccw interior rings
SELECT ST_IsPolygonCCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, ccw exerior ring, mixed interior rings
SELECT ST_IsPolygonCCW( 'POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, mixed interior rings
SELECT ST_IsPolygonCCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- MultiPolygon, ccw exterior rings only
SELECT ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 0, 101 1, 100 0)))'::geometry);
-- MultiPolygon, cw exterior rings only
SELECT ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 1, 1 0, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
-- MultiPolygon, mixed exterior rings
SELECT ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);

RETURN ST_IsPolygonCCW('POINT (0 0)'::geometry);
RETURN ST_IsPolygonCCW('MULTIPOINT ((0 0), (1 1))'::geometry);
RETURN ST_IsPolygonCCW('LINESTRING (1 1, 2 2)'::geometry);
RETURN ST_IsPolygonCCW('MULTILINESTRING ((1 1, 2 2), (3 3, 0 0))'::geometry);
-- EMPTY handling

RETURN ST_IsPolygonCCW('POLYGON EMPTY'::geometry);
-- Single polygon, ccw exterior ring only

RETURN ST_IsPolygonCCW('POLYGON ((0 0, 1 0, 1 1, 0 0))'::geometry);
-- Single polygon, cw exterior ring only

RETURN ST_IsPolygonCCW('POLYGON ((0 0, 1 1, 1 0, 0 0))'::geometry);
-- Single polygon, ccw exterior ring, cw interior rings

RETURN ST_IsPolygonCCW('POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, ccw interior rings

RETURN ST_IsPolygonCCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, ccw exerior ring, mixed interior rings

RETURN ST_IsPolygonCCW( 'POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 1 2, 2 2, 1 1), (5 5, 7 7, 5 7, 5 5))'::geometry);
-- Single polygon, cw exterior ring, mixed interior rings

RETURN ST_IsPolygonCCW( 'POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (1 1, 2 2, 1 2, 1 1), (5 5, 5 7, 7 7, 5 5))'::geometry);
-- MultiPolygon, ccw exterior rings only

RETURN ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 0, 101 1, 100 0)))'::geometry);
-- MultiPolygon, cw exterior rings only

RETURN ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 1, 1 0, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
-- MultiPolygon, mixed exterior rings

RETURN ST_IsPolygonCCW( 'MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((100 0, 101 1, 101 0, 100 0)))'::geometry);
--
-- ST_DistanceSpheroid
--
SELECT round(ST_DistanceSpheroid('MULTIPOLYGON(((-10 40,-10 55,-10 70,5 40,-10 40)))'::geometry,
		  'MULTIPOINT(20 40,20 55,20 70,35 40,35 55,35 70,50 40,50 55,50 70)',
			    'SPHEROID["GRS_1980",6378137,298.257222101]'));
SELECT round(ST_DistanceSpheroid('MULTIPOLYGON(((-10 40,-10 55,-10 70,5 40,-10 40)))'::geometry,
   'MULTIPOINT(20 40,20 55,20 70,35 40,35 55,35 70,50 40,50 55,50 70)'));
RETURN round(ST_DistanceSpheroid('MULTIPOLYGON(((-10 40,-10 55,-10 70,5 40,-10 40)))'::geometry,
   'MULTIPOINT(20 40,20 55,20 70,35 40,35 55,35 70,50 40,50 55,50 70)',
   'SPHEROID["GRS_1980",6378137,298.257222101]'));
RETURN round(ST_DistanceSpheroid('MULTIPOLYGON(((-10 40,-10 55,-10 70,5 40,-10 40)))'::geometry,
   'MULTIPOINT(20 40,20 55,20 70,35 40,35 55,35 70,50 40,50 55,50 70)'));

--
-- Temporal
--

--
-- ST_ClosestPointOfApproach
--
-- Converging
select ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 1, 10 10 10 10)'::geometry);
RETURN ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)'::geometry, 'LINESTRINGZM(0 0 0 1, 10 10 10 10)');
-- Following
select ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry);
RETURN ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry);
-- Crossing
select ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry);
RETURN ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry);
-- Meeting
select ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry);
RETURN ST_ClosestPointOfApproach( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry);
-- Disjoint
select ST_ClosestPointOfApproach( 'LINESTRINGM(0 0 0, 0 0 4)', 'LINESTRINGM(0 0 5, 0 0 10)'::geometry);
RETURN ST_ClosestPointOfApproach('LINESTRINGM(0 0 0, 0 0 4)', 'LINESTRINGM(0 0 5, 0 0 10)'::geometry);
--
-- ST_DistanceCPA
--
-- Converging
select ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 1, 10 10 10 10)'::geometry);
RETURN ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)'::geometry, 'LINESTRINGZM(0 0 0 1, 10 10 10 10)');
-- Following
select ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry);
RETURN ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 10 10 10 10)', 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry);
-- Crossing
select ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry);
RETURN ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry);
-- Meeting
select ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry);
RETURN ST_DistanceCPA( 'LINESTRINGZM(0 0 0 0, 0 0 0 10)', 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry);
-- Disjoint
select ST_DistanceCPA( 'LINESTRINGM(0 0 0, 0 0 4)', 'LINESTRINGM(0 0 5, 0 0 10)'::geometry);
RETURN ST_DistanceCPA('LINESTRINGM(0 0 0, 0 0 4)', 'LINESTRINGM(0 0 5, 0 0 10)'::geometry);
--
-- |=| Operator
--
-- Converging
select 'LINESTRINGZM(0 0 0 0, 10 10 10 10)' |=| 'LINESTRINGZM(0 0 0 1, 10 10 10 10)'::geometry;
RETURN 'LINESTRINGZM(0 0 0 0, 10 10 10 10)'::geometry |=| 'LINESTRINGZM(0 0 0 1, 10 10 10 10)';
-- Following
select 'LINESTRINGZM(0 0 0 0, 10 10 10 10)' |=| 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry;
RETURN 'LINESTRINGZM(0 0 0 0, 10 10 10 10)' |=| 'LINESTRINGZM(0 0 0 5, 10 10 10 15)'::geometry;
-- Crossing
select 'LINESTRINGZM(0 0 0 0, 0 0 0 10)' |=| 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry;
RETURN 'LINESTRINGZM(0 0 0 0, 0 0 0 10)' |=| 'LINESTRINGZM(-30 0 5 4, 10 0 5 6)'::geometry;
-- Meeting 
select 'LINESTRINGZM(0 0 0 0, 0 0 0 10)' |=| 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry;
RETURN 'LINESTRINGZM(0 0 0 0, 0 0 0 10)' |=| 'LINESTRINGZM(0 5 0 10, 10 0 5 11)'::geometry;
-- Disjoint
select 'LINESTRINGM(0 0 0, 0 0 4)' |=| 'LINESTRINGM(0 0 5, 0 0 10)'::geometry;
RETURN 'LINESTRINGM(0 0 0, 0 0 4)' |=| 'LINESTRINGM(0 0 5, 0 0 10)'::geometry;


--
-- ST_IsValidTrajectory
--
SELECT ST_IsValidTrajectory('POINTM(0 0 0)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGZ(0 0 0,1 1 1)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 0)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM(0 0 1,1 1 0)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1,1 1 2,1 0 1)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1)'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM EMPTY'::geometry);
SELECT ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1,1 1 2)'::geometry);

RETURN ST_IsValidTrajectory('POINTM(0 0 0)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGZ(0 0 0,1 1 1)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 0)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM(0 0 1,1 1 0)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1,1 1 2,1 0 1)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1)'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM EMPTY'::geometry);
RETURN ST_IsValidTrajectory('LINESTRINGM(0 0 0,1 1 1,1 1 2)'::geometry);

--
-- ST_CPAWithin
--
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 0.0);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1.0);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 0.5);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1.9);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2);
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2.0001);
-- temporary disjoint
SELECT ST_CPAWithin( 'LINESTRINGM(0 0 0, 10 0 10)'::geometry ,'LINESTRINGM(10 0 11, 10 10 20)'::geometry, 1e15);
SELECT ST_CPAWithin( 'LINESTRING(0 0 0, 1 0 0)'::geometry ,'LINESTRING(0 0 3 0, 1 0 2 1)'::geometry, 1e16);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 0.0);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1.0);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 0.5);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 1.9);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2);
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 1 0 1)'::geometry ,'LINESTRINGM(0 0 0, 1 0 1)'::geometry, 2.0001);
-- temporary disjoint
RETURN ST_CPAWithin( 'LINESTRINGM(0 0 0, 10 0 10)'::geometry ,'LINESTRINGM(10 0 11, 10 10 20)'::geometry, 1e15);
RETURN ST_CPAWithin( 'LINESTRING(0 0 0, 1 0 0)'::geometry ,'LINESTRING(0 0 3 0, 1 0 2 1)'::geometry, 1e16);
--
-- GEOS
--

--
-- ST_Intersection
--
SELECT ST_Intersection('MULTIPOINT ((0 0), (1 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry);
SELECT ST_AsEWKT(ST_Intersection('MULTIPOINT ((0 0), (1 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry));
RETURN ST_Intersection('MULTIPOINT ((0 0), (1 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry, -1.0);
RETURN ST_AsEWKT(ST_Intersection('MULTIPOINT ((0 0), (1 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry, -1.0));
RETURN ST_AsEWKT(ST_Intersection('MULTIPOINT ((2 2), (5 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry, -1.0));
RETURN ST_AsEWKT(ST_Intersection('MULTIPOINT ((2 2), (5 1))'::geometry, 'MULTIPOINT ((0 0), (1 1))'::geometry));

--
-- Algorithms
-- 

--
-- ST_Simplify
--
SELECT ST_AsEWKT(ST_Simplify('POLYGON((0 0,1 1,1 3,0 4,-2 3,-1 1,0 0))'::geometry, 1));
SELECT ST_AsEWKT(ST_Simplify('POLYGON((0 0,1 1,1 3,2 3,2 0,0 0))'::geometry, 1));

RETURN ST_AsEWKT(ST_Simplify('POLYGON((0 0,1 1,1 3,0 4,-2 3,-1 1,0 0))', 1));
RETURN ST_AsEWKT(ST_Simplify('POLYGON((0 0,1 1,1 3,2 3,2 0,0 0))'::geometry, 1));

/*
 * 3D Functions
 */
--3D Distance functions


CYPHER WITH 'POINT(1 1 1)'::geometry as a, 'POINT(3 2 7)'::geometry as b
RETURN 	ST_3DDistance(a,b),
		    ST_3DMaxDistance(a,b)::numeric,
			ST_3DDWithin(a,b,5),
			ST_3DDFullyWithin(a,b,5),
			ST_ASEWKT(ST_3DShortestline(a,b)),
			ST_ASEWKT(ST_3DClosestpoint(a,b)),
			ST_ASEWKT(ST_3DLongestline(a,b));

CYPHER WITH 'POINT(1 1 1)'::geometry as a, 'LINESTRING(0 0 0, 2 2 2)'::geometry as b
RETURN 	ST_3DDistance(a,b),
		    ST_3DMaxDistance(a,b)::numeric,
			ST_3DDWithin(a,b,5),
			ST_3DDFullyWithin(a,b,5),
			ST_ASEWKT(ST_3DShortestline(a,b)),
			ST_ASEWKT(ST_3DClosestpoint(a,b)),
			ST_ASEWKT(ST_3DLongestline(a,b));

CYPHER WITH 'POINT(1 1 1)'::geometry as a, 'LINESTRING(5 2 6, -3 -2 4)'::geometry as b
RETURN 	ST_3DDistance(a,b),
		    ST_3DMaxDistance(a,b)::numeric,
			ST_3DDWithin(a,b,5),
			ST_3DDFullyWithin(a,b,5),
			ST_ASEWKT(ST_3DShortestline(a,b)),
			ST_ASEWKT(ST_3DClosestpoint(a,b)),
			ST_ASEWKT(ST_3DLongestline(a,b));



CYPHER WITH 'LINESTRING(1 1 3, 5 7 8)'::geometry as a, 'POINT(1 1 1)'::geometry as b
RETURN 	ST_3DDistance(a,b),
		    ST_3DMaxDistance(a,b)::numeric,
			ST_3DDWithin(a,b,5),
			ST_3DDFullyWithin(a,b,5),
			ST_ASEWKT(ST_3DShortestline(a,b)),
			ST_ASEWKT(ST_3DClosestpoint(a,b)),
			ST_ASEWKT(ST_3DLongestline(a,b));

CYPHER WITH 'LINESTRING(1 0 5, 11 0 5)'::geometry as a, 'LINESTRING(5 2 0, 5 2 10, 5 0 13)'::geometry as b
RETURN 	ST_3DDistance(a,b),
		    ST_3DMaxDistance(a,b)::numeric,
			ST_3DDWithin(a,b,5),
			ST_3DDFullyWithin(a,b,5),
			ST_ASEWKT(ST_3DShortestline(a,b)),
			ST_ASEWKT(ST_3DClosestpoint(a,b)),
			ST_ASEWKT(ST_3DLongestline(a,b));

CYPHER WITH 'LINESTRING(1 1 1 , 2 2 2)'::geometry as a, 'POLYGON((0 0 0, 2 2 2, 3 3 3, 0 0 0))'::geometry as b
RETURN 	ST_3DDistance(a,b);


CYPHER WITH 'LINESTRING(1 1 1 , 2 2 2)'::geometry as a, 'POLYGON((0 0 0, 2 2 2, 3 3 1, 0 0 0))'::geometry as b
RETURN 	ST_3DDistance(a,b);


-- 3D mixed dimmentionality #2034
--closestpoint with 2d as first point and 3d as second

RETURN st_astext(st_3dclosestpoint('linestring(0 0,1 1,2 0)'::geometry, 'linestring(0 2 3, 3 2 3)'::geometry));
--closestpoint with 3d as first point and 2d as second

RETURN st_astext(st_3dclosestpoint('linestring(0 0 1,1 1 2,2 0 3)'::geometry, 'linestring(0 2, 3 2)'::geometry));
--shortestline with 2d as first point and 3d as second

RETURN st_astext(st_3dshortestline('linestring(0 0,1 1,2 0)'::geometry, 'linestring(0 2 3, 3 2 3)'::geometry));
--shortestline with 3d as first point and 2d as second

RETURN st_astext(st_3dshortestline('linestring(0 0 1,1 1 2,2 0 3)'::geometry, 'linestring(0 2, 3 2)'::geometry));

--distance with 2d as first point and 3d as second

RETURN st_3ddistance('linestring(0 0,1 1,2 0)'::geometry, 'linestring(0 2 3, 3 2 3)'::geometry);
--distance with 3d as first point and 2d as second

RETURN st_3ddistance('linestring(0 0 1,1 1 2,2 0 3)'::geometry, 'linestring(0 2, 3 2)'::geometry);

RETURN ST_AsText(ST_3DClosestPoint('POINT(0 0 0)', 'POINT(0 0)'));
RETURN ST_AsText(ST_3DShortestLine('LINESTRING(2 1, 3 0)', 'LINESTRING(0 0 2, 3 3 -4)'));

/*
 * Polyheadral Surface
 */
 -- ST_Dimension on 2D: not closed

RETURN ST_Dimension('POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0)))'::geometry);
RETURN ST_Dimension('GEOMETRYCOLLECTION(POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0))))'::geometry);
-- ST_Dimension on 3D: closed

RETURN ST_Dimension('POLYHEDRALSURFACE(((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,1 0 0,0 0 1,0 0 0)),((1 0 0,0 1 0,0 0 1,1 0 0)))'::geometry);
RETURN  ST_Dimension('GEOMETRYCOLLECTION(POLYHEDRALSURFACE(((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,1 0 0,0 0 1,0 0 0)),((1 0 0,0 1 0,0 0 1,1 0 0))))'::geometry);
-- ST_Dimension on 4D: closed

RETURN ST_Dimension('POLYHEDRALSURFACE(((0 0 0 0,0 0 1 0,0 1 0 2,0 0 0 0)),((0 0 0 0,0 1 0 0,1 0 0 4,0 0 0 0)),((0 0 0 0,1 0 0 0,0 0 1 6,0 0 0 0)),((1 0 0 0,0 1 0 0,0 0 1 0,1 0 0 0)))'::geometry);
RETURN ST_Dimension('GEOMETRYCOLLECTION(POLYHEDRALSURFACE(((0 0 0 0,0 0 1 0,0 1 0 2,0 0 0 0)),((0 0 0 0,0 1 0 0,1 0 0 4,0 0 0 0)),((0 0 0 0,1 0 0 0,0 0 1 6,0 0 0 0)),((1 0 0 0,0 1 0 0,0 0 1 0,1 0 0 0))))'::geometry);
-- ST_Dimension on 3D: invalid polyedron (a single edge is shared 3 times)

RETURN ST_Dimension('POLYHEDRALSURFACE(((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,0 1 0,0 0 1,0 0 0)),((1 0 0,0 1 0,0 0 1,1 0 0)))'::geometry);
-- ST_Dimension on 3D: invalid polyedron (redundant point inside each face)

RETURN ST_Dimension('POLYHEDRALSURFACE(((0 0 0,1 0 0,1 0 0,0 0 0)),((0 0 1,1 0 1,1 0 1,0 0 1)),((0 0 2,1 0 2,1 0 2,0 0 2)),((0 0 3,1 0 3,1 0 3,0 0 3)))'::geometry);
-- ST_NumPatches

RETURN ST_NumPatches('POLYHEDRALSURFACE EMPTY'::geometry);
RETURN ST_NumPatches('POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0)))'::geometry);
RETURN ST_NumPatches('POLYHEDRALSURFACE(((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,1 0 0,0 0 1,0 0 0)),((1 0 0,0 1 0,0 0 1,1 0 0)))'::geometry);
-- ST_PatchN

RETURN ST_AsEWKT(ST_patchN('POLYHEDRALSURFACE EMPTY'::geometry, 1));
RETURN ST_AsEWKT(ST_patchN('POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0)))'::geometry, 1));
RETURN ST_AsEWKT(ST_patchN('POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0)))'::geometry, 0));
RETURN ST_AsEWKT(ST_patchN('POLYHEDRALSURFACE(((0 0,0 0,0 1,0 0)))'::geometry, 2));
RETURN ST_AsEWKT(ST_patchN('POLYHEDRALSURFACE(((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,1 0 0,0 0 1,0 0 0)),((1 0 0,0 1 0,0 0 1,1 0 0)))'::geometry, 2));
-- PolyedralSurface (TODO)

RETURN ST_AsText(ST_Reverse('POLYHEDRALSURFACE EMPTY'::geometry));
RETURN ST_AsText(ST_Reverse('POLYHEDRALSURFACE (((0 0,0 0,0 1,0 0)),((0 0,0 1,1 0,0 0)),((0 0,1 0,0 0,0 0)),((1 0,0 1,0 0,1 0)))'::geometry));

RETURN ST_AsText(ST_ShiftLongitude('SRID=4326;POINT(270 0)'::geometry));
RETURN ST_AsText(ST_ShiftLongitude('SRID=4326;POINT(-90 0)'::geometry));
RETURN ST_AsText(ST_ShiftLongitude('SRID=4326;LINESTRING(174 12, 182 13)'::geometry));

-- TODO KNN

    WITH ST_GeomFromText(
'PolyhedralSurface(
((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)),
((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)), ((0 0 0, 1 0 0, 1 0 1, 0 0 1, 0 0 0)),  ((1 1 0, 1 1 1, 1 0 1, 1 0 0, 1 1 0)),
((0 1 0, 0 1 1, 1 1 1, 1 1 0, 0 1 0)),  ((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1))
)') as a
RETURN ST_Translate(a,100, 450,1000);

    WITH ST_GeomFromText(
'PolyhedralSurface(
((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)),
((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )') as a
RETURN ST_Translate(a,100, 450,1000);

-- #68 --
--RETURN '#68a', ST_AsText(ST_ShiftLongitude(ST_GeomFromText('MULTIPOINT(1 3, 4 5)')));
--RETURN '#68b', ST_AsText(ST_ShiftLongitude(ST_GeomFromText('CIRCULARSTRING(1 3, 4 5, 6 7)')));

CREATE (:j {j: 'PolyhedralSurface(((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )'::geometry })  ;
CREATE (:j {j: 'PolyhedralSurface(
((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)),
((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)), ((0 0 0, 1 0 0, 1 0 1, 0 0 1, 0 0 0)),  ((1 1 0, 1 1 1, 1 0 1, 1 0 0, 1 1 0)),
((0 1 0, 0 1 1, 1 1 1, 1 1 0, 0 1 0)),  ((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1))
)'::geometry })  ;
SET enable_mergejoin = ON;
SET enable_hashjoin = ON;
SET enable_nestloop = ON;
SET enable_seqscan = false;
SET enable_sort = false;
CREATE INDEX ON postgis.j USING gist((properties->'"j"') gist_geometry_ops_nd);
\d+ postgis.j

EXPLAIN (costs off)

    MATCH (a:j)
    WHERE a.j &&& 'PolyhedralSurface(((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )'::geometry
RETURN a;

    MATCH (a:j)
    WHERE a.j &&& 'PolyhedralSurface(((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )'::geometry
RETURN a;
EXPLAIN (costs off)

    MATCH (a:j)
RETURN a ORDER BY a.j <<->> 'PolyhedralSurface(((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )';

    MATCH (a:j)
RETURN a ORDER BY a.j <<->> 'PolyhedralSurface(((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)) )';
--
-- 2D Gist Indices
--
CREATE (:i {i: 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry });
CREATE (:i {i: 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry }) ;
CREATE (:i {i: 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry })  ;
SELECT create_vlabel('postgis', 'i');
CREATE INDEX ON postgis.i USING gist ((properties->'"i"') gist_geometry_ops_2d);
CREATE (:i {i: 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry });
/*






    MATCH (i:i) 
    WHERE i.i << 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry
RETURN i;
EXPLAIN

    MATCH (i:i) 
    WHERE i.i << 'POLYGON( (0 0, 10 0, 10 10, 0 10, 0 0) )'::geometry
RETURN i;
*/








--
-- Typecasting
--

--
-- To Geometry
--
RETURN topoint('(1, 2)')::geometry ;
RETURN topath('(1, 2), (3, 4)')::geometry ;
RETURN topolygon('(1,1), (2,2), (3, 3), (4, 4)')::geometry ;
SELECT 'POLYGON((0 0,1 1,1 3,2 3,2 0,0 0))'::geometry::gtype;

RETURN topoint('POINT(1 1)'::geometry);

RETURN 'POINT(1 1)'::geometry;
--
-- From Box3D
--
-- gtype Box3D to box3d
RETURN toBox3D('BOX3D(1 2 3, 4 5 6)');
-- gtype string to box3d
RETURN 'BOX3D(1 2 3, 4 5 6)' ;
-- gtype Box3D to gtype box2d
RETURN toBox3D('BOX3D(1 2 3, 4 5 6)')::box2d ;
-- gtype Box3D to gtype geometry
RETURN toBox3D('BOX3D(1 2 3, 4 5 6)')::geometry;
-- gtype Box3D to geometry 
RETURN toBox3D('BOX3D(1 2 3, 4 5 6)');
--
-- From Box2D
--
-- gtype Box2d to Box2d
RETURN toBox2D('box(1 2, 5 6)') ;
-- gtype string to Box2d
RETURN 'box(1 2, 5 6)' ;
-- gtype Box2d to geometry
RETURN toBox2D('box(1 2, 5 6)') ;
-- gtype Box3D to box2d
RETURN toBox3D('BOX3D(1 2 3, 4 5 6)') ;

DROP GRAPH postgis CASCADE;