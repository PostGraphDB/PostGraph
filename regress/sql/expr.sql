
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

CREATE GRAPH expr;
USE GRAPH expr;

--
-- a bunch of comparison operators
--
RETURN 1 = 1.0;
RETURN 1 > -1.0;
RETURN -1.0 < 1;
RETURN 'aaa' < 'z';
RETURN 'z' > 'aaa';
RETURN false = false;
RETURN ('string' < true);
RETURN true < 1;
RETURN (1 + 1.0) = (7 % 5);

-- IS NULL
RETURN null IS NULL;
RETURN 1 IS NULL;
-- IS NOT NULL
RETURN 1 IS NOT NULL;
RETURN null IS NOT NULL;
-- NOT
RETURN NOT false;
RETURN NOT true;
-- AND
RETURN true AND true;
RETURN true AND false;
RETURN false AND true;
RETURN false AND false;
-- OR
RETURN true OR true;
RETURN true OR false;
RETURN false OR true;
RETURN false OR false;
-- The ONE test on operator precedence...
RETURN NOT ((true OR false) AND (false OR true));
-- XOR
RETURN true XOR true;
RETURN true XOR false;
RETURN false XOR true;
RETURN false XOR false;

--
-- String Concat
--
RETURN 'str' + 'str';
RETURN 'str' + 1;
RETURN 'str' + 1.0;
RETURN 1 + 'str';
RETURN 1.0 + 'str';

--
-- Operator Precedence
--
RETURN (-(3 * 2 - 4.0) ^ ((10 / 5) + 1)) % -3;

--
-- indirection object.property, object['property'], and array[element]
--
RETURN [ 1, { bool: true, int: 3, array: [ 9, 11, { boom: false, float: 3.14 }, 13 ] }, 5, 7, 9 ][1].array[2]['float'];


-- coalesce
RETURN coalesce(null, 1, null, null);
RETURN coalesce(null, -3.14, null, null);
RETURN coalesce(null, 'string', null, null);
RETURN coalesce(null, null, null, []);
RETURN coalesce(null, null, null, {});
RETURN coalesce(null);


--
-- STARTS WITH, ENDS WITH, and CONTAINS
--
RETURN 'abcdefghijklmnopqrstuvwxyz' STARTS WITH 'abcd';
RETURN 'abcdefghijklmnopqrstuvwxyz' ENDS WITH 'wxyz';
RETURN 'abcdefghijklmnopqrstuvwxyz' CONTAINS 'klmn';
RETURN 'abcdefghijklmnopqrstuvwxyz' STARTS WITH 'bcde';
RETURN 'abcdefghijklmnopqrstuvwxyz' ENDS WITH 'vwxy';
RETURN 'abcdefghijklmnopqrstuvwxyz' CONTAINS 'klmo';
RETURN 'abcdefghijklmnopqrstuvwxyz' STARTS WITH NULL;
RETURN 'abcdefghijklmnopqrstuvwxyz' ENDS WITH NULL;
RETURN 'abcdefghijklmnopqrstuvwxyz' CONTAINS NULL;

-- size() of a string
RETURN size('12345');
RETURN size('1234567890');


-- reverse
RETURN reverse('gnirts a si siht');
RETURN reverse(null);
RETURN reverse([4923, 'abc', 521, NULL, 487]);
RETURN reverse([4923]);
RETURN reverse([4923, 257]);
RETURN reverse([4923, 257, null]);
RETURN reverse([4923, 257, 'tea']);
RETURN reverse([[1, 4, 7], 4923, [1, 2, 3], 'abc', 521, NULL, 487, ['fgt', 7, 10]]);
RETURN reverse([4923, 257, {test1: 'key'}]);
RETURN reverse([4923, 257, {test2: [1, 2, 3]}]);
RETURN reverse(true);
RETURN reverse(3.14);
-- toUpper
RETURN toUpper('to uppercase');
RETURN toUpper(null);
RETURN toUpper('');
RETURN toUpper(true);
-- toLower
RETURN toLower('TO LOWERCASE');
RETURN toLower(null);
RETURN toLower(true);
-- lTrim
RETURN lTrim('  string   ');
RETURN lTrim(null);
RETURN lTrim(true);
-- rtrim
RETURN rTrim('  string   ');
RETURN rTrim(null);
RETURN rTrim(true);
-- trim
RETURN trim('  string   ');
RETURN trim(null);
RETURN trim(true);
-- left
RETURN left('123456789', 1);
RETURN left('123456789', 3);
RETURN left('123456789', 0);
RETURN left(null, 1);
RETURN left(null, null);
RETURN left('123456789', null);
RETURN left('123456789', -1);
--right
RETURN right('123456789', 1);
RETURN right('123456789', 3);
RETURN right('123456789', 0);
RETURN right(null, 1);
RETURN right(null, null);
RETURN right('123456789', null);
RETURN right('123456789', -1);
-- substring
RETURN substring('0123456789', 0, 1);
RETURN substring('0123456789', 1, 3);
RETURN substring('0123456789', 3);
RETURN substring('0123456789', 0);
RETURN substring(null, null, null);
RETURN substring(null, null);
RETURN substring(null, 1);
RETURN substring('123456789', null);
RETURN substring('123456789', 0, -1);
RETURN substring('123456789', -1);
RETURN substring('123456789');
-- split
RETURN split('a,b,c,d,e,f', ',');
RETURN split('a,b,c,d,e,f', '');
RETURN split('a,b,c,d,e,f', ' ');
RETURN split('a,b,cd  e,f', ' ');
RETURN split('a,b,cd  e,f', '  ');
RETURN split('a,b,c,d,e,f', 'c,');
RETURN split(null, null);
RETURN split('a,b,c,d,e,f', null);
RETURN split(null, ',');
RETURN split(123456789, ',');
RETURN split('a,b,c,d,e,f', -1);
RETURN split('a,b,c,d,e,f');
-- replace
RETURN replace('Hello', 'lo', 'p');
RETURN replace('Hello', 'hello', 'Good bye');
RETURN replace('abcabcabc', 'abc', 'a');
RETURN replace('abcabcabc', 'ab', '');
RETURN replace('ababab', 'ab', 'ab');
RETURN replace(null, null, null);
RETURN replace('Hello', null, null);
RETURN replace('Hello', '', null);
RETURN replace('', '', '');
RETURN replace('Hello', 'Hello', '');
RETURN replace('', 'Hello', 'Mellow');
RETURN replace('Hello');
RETURN replace('Hello', null);
RETURN replace('Hello', 'e', 1);
RETURN replace('Hello', 1, 'e');
-- initCap
RETURN initCap('hello world');


--
-- Math
--
-- sin
RETURN sin(0);
-- cos
RETURN cos(null);
RETURN cos(0);
-- tan
RETURN tan(null);
RETURN tan(0);
-- cot
RETURN cot(null);
RETURN cot(0);
-- asin
RETURN asin(1)*2;
RETURN asin(1.1);
RETURN asin(-1.1);
RETURN asin(null);
RETURN asin('0');
-- acos
RETURN acos(0)*2;
RETURN acos(1.1);
RETURN acos(-1.1);
RETURN acos(null);
RETURN acos('0');
-- atan
RETURN atan(1)*4;
RETURN atan(null);
RETURN atan('0');
-- atan2
RETURN atan2(1, 1)*4;
RETURN atan2(null, null);
RETURN atan2(null, 1);
RETURN atan2(1, null);
RETURN atan2('0', 1);
RETURN atan2(0, '1');
-- pi
RETURN pi();
RETURN sin(pi());
RETURN sin(pi()/4);
RETURN cos(pi());
RETURN cos(pi()/2);
RETURN sin(pi()/2);
-- radians
RETURN radians(0);
RETURN radians(360);
RETURN radians(180);
RETURN radians(90);
RETURN radians(null);
RETURN radians('1');
-- degrees
RETURN degrees(0);
RETURN degrees(2*pi());
RETURN degrees(pi());
RETURN degrees(pi()/2);
RETURN degrees(null);
RETURN degrees('1');
--sinh
RETURN sinh(3.1415);
RETURN sinh(pi());
RETURN sinh(0);
RETURN sinh(1);
--cosh
RETURN cosh(3.1415);
RETURN cosh(pi());
RETURN cosh(0);
RETURN cosh(1);
--tanh
RETURN tanh(3.1415);
RETURN tanh(pi());
RETURN tanh(0);
RETURN tanh(1);
--asinh
RETURN asinh(3.1415);
RETURN asinh(pi());
RETURN asinh(0);
RETURN asinh(1);
--acosh
RETURN acosh(3.1415);
RETURN acosh(pi());
RETURN acosh(0);
RETURN acosh(1);
--atanh
RETURN atanh(3.1415);
RETURN atanh(pi());
RETURN atanh(0);
RETURN atanh(1);
-- abs
RETURN abs(0);
RETURN abs(10);
RETURN abs(-10);
RETURN abs(null);
-- ceil
RETURN ceil(0);
RETURN ceil(1);
RETURN ceil(-1);
RETURN ceil(1.01);
RETURN ceil(-1.01);
RETURN ceiling(-1.01);
RETURN ceil(-1.01::numeric);
RETURN ceil('1');
RETURN ceil(null);
-- floor
RETURN floor(0);
RETURN floor(1);
RETURN floor(-1);
RETURN floor(1.01);
RETURN floor(-1.01);
RETURN floor(null);
RETURN floor('1');
-- round
RETURN round(0);
RETURN round(4.49999999);
RETURN round(4.5);
RETURN round(-4.49999999);
RETURN round(-4.5);
RETURN round(7.4163, 3);
RETURN round(7.416343479, 8);
RETURN round(7.416343479, NULL);
RETURN round('1');
RETURN round(NULL, 7);
RETURN round(7, 2);
RETURN round(7.4342, 2.1123);
RETURN round(NULL, NULL);
-- sign
RETURN sign(10);
RETURN sign(-10);
RETURN sign(0);
RETURN sign('1');
RETURN sign(null);
-- gcd
RETURN gcd(10, 5);
RETURN gcd(10.0, 5.0);
RETURN gcd(10.0, 5);
RETURN gcd(10, 5::numeric);
RETURN gcd('10', 5);
-- lcm
RETURN lcm(10, 5);
RETURN lcm(10.0, 5.0);
RETURN lcm(10.0, 5);
RETURN lcm('10', 5);
RETURN lcm(10::numeric, '5');
-- log (ln) 
RETURN log(2.718281828459045);
RETURN log(10);
RETURN log(null);
RETURN log(0);
RETURN log(-1);
-- log10
RETURN log10(10);
RETURN log10(null);
RETURN log10(0);
RETURN log10(-1);
-- e
RETURN e();
RETURN log(e());
-- exp
RETURN exp(1);
RETURN exp(0);
RETURN exp(null);
RETURN exp('1');
-- sqrt()
RETURN sqrt(25);
RETURN sqrt(1);
RETURN sqrt(0);
RETURN sqrt(-1);
RETURN sqrt(null);
RETURN sqrt('1');
-- cbrt()
RETURN cbrt(125);
RETURN cbrt(1);
RETURN cbrt(0);
RETURN cbrt(-1);
RETURN cbrt(null);
RETURN cbrt('1');
-- factorial
RETURN factorial(10);


--
-- Typecasting Functions
--
-- toBoolean
RETURN toBoolean(true);
RETURN toBoolean(false);
RETURN toBoolean('true');
RETURN toBoolean('false');
RETURN toBoolean('falze');
RETURN toBoolean(null);
RETURN toBoolean(1);
-- toFloat
RETURN toFloat(1);
RETURN toFloat(1.2);
RETURN toFloat('1');
RETURN toFloat('1.2');
RETURN toFloat('1.2'::numeric);
RETURN toFloat('falze');
RETURN toFloat(null);
RETURN toFloat(true);
-- toInteger
RETURN toInteger(1);
RETURN toInteger(1.2);
RETURN toInteger('1');
RETURN toInteger('1.2');
RETURN toInteger('1.2'::numeric);
RETURN toInteger('falze');
RETURN toInteger(null);
RETURN toInteger(true);
-- toString
RETURN toString(3);
RETURN toString(3.14);
RETURN toString(3.14::float);
RETURN toString(3.14::numeric);
RETURN toString(true);
RETURN toString(false);
RETURN toString('a string');
RETURN toString(null);


DROP GRAPH expr;