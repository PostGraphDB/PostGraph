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

LOAD 'postgraph';
SET search_path TO postgraph;
set timezone TO 'GMT';

CREATE GRAPH range;
USE GRAPH range;

RETURN intrange(0, 1);
RETURN intrange(0, 1, '()');
RETURN intrange(0, 1, '(]');
RETURN intrange(0, 1, '[)');
RETURN intrange(0, 1, '[]');

--
-- intrange = intrange
--
RETURN intrange(0, 1, '[]') = intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') = intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') = intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') = intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') = intrange(3, 4, '[]');

--
-- intrange <> intrange
--
RETURN intrange(0, 1, '[]') <> intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') <> intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') <> intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') <> intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') <> intrange(3, 4, '[]');


--
-- intrange > intrange
--
RETURN intrange(0, 1, '[]') > intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') > intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') > intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') > intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') > intrange(3, 4, '[]');

--
-- intrange >= intrange
--
RETURN intrange(0, 1, '[]') >= intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') >= intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') >= intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') >= intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') >= intrange(3, 4, '[]');


--
-- intrange < intrange
--
RETURN intrange(0, 1, '[]') < intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') < intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') < intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') < intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') < intrange(3, 4, '[]');


--
-- intrange <= intrange
--
RETURN intrange(0, 1, '[]') <= intrange(0, 1, '[]');
RETURN intrange(0, 1, '()') <= intrange(0, 1, '[]');
RETURN intrange(0, 1, '(]') <= intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') <= intrange(0, 1, '[]');
RETURN intrange(0, 1, '[)') <= intrange(3, 4, '[]');

RETURN numrange(0, 1);
RETURN numrange(0, 1, '()');
RETURN numrange(0, 1, '(]');
RETURN numrange(0, 1, '[)');
RETURN numrange(0, 1, '[]');

--
-- numrange = numrange
--
RETURN numrange(0, 1, '[]') = numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') = numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') = numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') = numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') = numrange(3, 4, '[]');

--
-- numrange <> numrange
--
RETURN numrange(0, 1, '[]') <> numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') <> numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') <> numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') <> numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') <> numrange(3, 4, '[]');


--
-- numrange > numrange
--
RETURN numrange(0, 1, '[]') > numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') > numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') > numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') > numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') > numrange(3, 4, '[]');

--
-- numrange >= numrange
--
RETURN numrange(0, 1, '[]') >= numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') >= numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') >= numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') >= numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') >= numrange(3, 4, '[]');

--
-- numrange < numrange
--
RETURN numrange(0, 1, '[]') < numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') < numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') < numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') < numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') < numrange(3, 4, '[]');

--
-- numrange <= numrange
--
RETURN numrange(0, 1, '[]') <= numrange(0, 1, '[]');
RETURN numrange(0, 1, '()') <= numrange(0, 1, '[]');
RETURN numrange(0, 1, '(]') <= numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') <= numrange(0, 1, '[]');
RETURN numrange(0, 1, '[)') <= numrange(3, 4, '[]');

RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]');

--
-- tsrange = tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') ;
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');

--
-- tsrange <> tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <> tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tsrange < tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') < tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') < tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') < tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') < tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tsrange <= tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tsrange > tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') > tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') > tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') > tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') > tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tsrange >= tsrange
--
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') >= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') >= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') >= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') >= tsrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');

RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]');

--
-- tstzrange = tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');

--
-- tstzrange <> tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <> tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tstzrange < tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') < tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') < tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') < tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') < tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');

--
-- tstzrange <= tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') <= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') <= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') <= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') <= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tstzrange > tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') > tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') > tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') > tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') > tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');


--
-- tstzrange >= tstzrange
--
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()') >= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '(]') >= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[)') >= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');
RETURN tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '[]') >= tstzrange('1/1/2000 12:00:00', '1/1/2000 4:00:00 PM', '()');

RETURN daterange('1/1/2000', '1/1/2001');
RETURN daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]');
RETURN daterange('1/1/2000', '1/1/2001', '[)');
RETURN daterange('1/1/2000', '1/1/2001', '[]');

--
-- rangedate = rangedate
--
RETURN daterange('1/1/2000', '1/1/2001', '()') = daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') = daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') = daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') = daterange('1/1/2000', '1/1/2001', '()');

--
-- rangedate <> rangedate
--
RETURN daterange('1/1/2000', '1/1/2001', '()') <> daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') <> daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') <> daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') <> daterange('1/1/2000', '1/1/2001', '()');

--
-- rangedate > rangedate
--

RETURN daterange('1/1/2000', '1/1/2001', '()') > daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') > daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') > daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') > daterange('1/1/2000', '1/1/2001', '()');

--
-- rangedate >= rangedate
--
RETURN daterange('1/1/2000', '1/1/2001', '()') >= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') >= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') >= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') >= daterange('1/1/2000', '1/1/2001', '()');

--
-- rangedate < rangedate
--
RETURN daterange('1/1/2000', '1/1/2001', '()') < daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') < daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') < daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') < daterange('1/1/2000', '1/1/2001', '()');

--
-- rangedate <= rangedate
--
RETURN daterange('1/1/2000', '1/1/2001', '()') <= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '(]') <= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[)') <= daterange('1/1/2000', '1/1/2001', '()');
RETURN daterange('1/1/2000', '1/1/2001', '[]') <= daterange('1/1/2000', '1/1/2001', '()');



--
-- Clean up
--
DROP GRAPH range;
