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

CREATE GRAPH network;
USE GRAPH network;

--
-- inet
--
RETURN toinet('::ffff:fff0:1');
RETURN 192.168.1.5;
RETURN 192.168.1/24;
RETURN '::ffff:fff0:1'::inet;

SELECT '192.168.1.5'::gtype;
SELECT '192.168.1.5/24'::gtype;
RETURN 192.168.1.5;

--
-- cidr
--
RETURN tocidr(192.168.1.5);
RETURN tocidr(192.168.1/24);
RETURN tocidr('::ffff:fff0:1');
RETURN 192.168.1.5::cidr;
RETURN 192.168.1/24::cidr;
RETURN '::ffff:fff0:1'::cidr;

--
-- macaddr
--
RETURN tomacaddr('12:34:56:78:90:ab');
RETURN '12:34:56:78:90:ab'::macaddr;

--
-- macaddr8
--
RETURN tomacaddr8('12:34:56:78:90:ab:cd:ef');
RETURN '12:34:56:78:90:ab:cd:ef'::macaddr8;

--
-- inet typecasting
--
-- PG inet -> gtype inet
SELECT '192.168.1.5'::inet::gtype;
-- gtype inet -> PG inet
SELECT toinet('"192.168.1.5"'::gtype)::inet;
-- inet -> inet
RETURN toinet('::ffff:fff0:1')::inet;
-- string -> inet
SELECT '"192.168.1.5"'::gtype::inet;

-- type coercion
RETURN 192.168.1.5;
RETURN toinet('192.168.1.5');
RETURN '192.168.1.5'::inet;
RETURN '192.168.1.5';

--
-- cidr typecasting
--
-- PG cidr -> gtype cidr
SELECT '192.168.1.5'::cidr::gtype;
-- gtype cidr -> PG cidr
SELECT tocidr('"192.168.1.5"'::gtype)::cidr;
-- cidr -> cidr
RETURN tocidr('::ffff:fff0:1')::cidr;


-- type coercion
RETURN tocidr(192.168.1.5);
RETURN 192.168.1.5::cidr;
RETURN '192.168.1.5';

--
-- macaddr typecasting
--
-- macaddr -> macaddr
RETURN tomacaddr('12:34:56:78:90:ab')::macaddr;
-- macaddr8 -> macaddr
RETURN tomacaddr8('12:34:56:FF:FE:ab:cd:ef')::macaddr;

--
-- macaddr8 typecasting
--
-- macaddr8 -> macaddr8
RETURN tomacaddr8('12:34:56:78:90:ab')::macaddr8;
-- macaddr -> macaddr8
RETURN tomacaddr('12:34:56:78:90:ab')::macaddr8;

--
-- inet = inet
--
RETURN 192.168.1.5 = 192.168.1.5;
RETURN 192.168.1.4 = 192.168.1.5;
RETURN 192.168.1.6 = 192.168.1.5;

--
-- inet <> inet
--
RETURN 192.168.1.5 <> 192.168.1.5;
RETURN 192.168.1.4 <> 192.168.1.5;
RETURN 192.168.1.6 <> 192.168.1.5;

--
-- inet > inet
--
RETURN 192.168.1.5 > 192.168.1.5;
RETURN 192.168.1.4 > 192.168.1.5;
RETURN 192.168.1.6 > 192.168.1.5;

--
-- inet >= inet
--
RETURN 192.168.1.5 >= 192.168.1.5;
RETURN 192.168.1.4 >= 192.168.1.5;
RETURN 192.168.1.6 >= 192.168.1.5;

--
-- inet < inet
--
RETURN 192.168.1.5 < 192.168.1.5;
RETURN 192.168.1.4 < 192.168.1.5;
RETURN 192.168.1.6 < 192.168.1.5;

--
-- inet <= inet
--
RETURN 192.168.1.5 <= 192.168.1.5;
RETURN 192.168.1.4 <= 192.168.1.5;
RETURN 192.168.1.6 <= 192.168.1.5;


--
-- cidr = cidr
--
RETURN 192.168.1.5::cidr = 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr = 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr = 192.168.1.5::cidr;

--
-- cidr <> cidr
--
RETURN 192.168.1.5::cidr <> 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr <> 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr <> 192.168.1.5::cidr;

--
-- cidr > cidr
--
RETURN 192.168.1.5::cidr > 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr > 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr > 192.168.1.5::cidr;

--
-- cidr >= cidr
--
RETURN 192.168.1.5::cidr >= 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr >= 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr >= 192.168.1.5::cidr;

--
-- cidr < cidr
--
RETURN 192.168.1.5::cidr < 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr < 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr < 192.168.1.5::cidr;

--
-- cidr <= cidr
--
RETURN 192.168.1.5::cidr <= 192.168.1.5::cidr;
RETURN 192.168.1.4::cidr <= 192.168.1.5::cidr;
RETURN 192.168.1.6::cidr <= 192.168.1.5::cidr;



--
-- inet + integer
--
RETURN 192.168.1.5 + 10;

--
-- inet - integer
--
RETURN 192.168.1.5 - 10;

--
-- integer + inet
--
RETURN 10 + 192.168.1.5;

--
-- inet - inet
--
RETURN 192.168.1.5 - 192.168.1.0;

--
-- ~ inet
--
RETURN ~ 192.168.1.5;

--
-- inet & inet
--
RETURN 10.1.0.0/32 & 192.168.1.5;
RETURN 192.168.1.5 & 192.168.1.5;

--
-- inet | inet
--
RETURN 10.1.0.0/32 | 192.168.1.5;
RETURN 192.168.1.5 | 192.168.1.5;

--
-- inet << inet
--
RETURN 192.168.1.5 << 192.168.1/24;
RETURN 192.168.0.5 << 192.168.1/24;
RETURN 192.168.1/24 << 192.168.1/24;

--
-- inet <<= inet
--
RETURN 192.168.1.5 <<= 192.168.1/24;
RETURN 192.168.0.5 <<= 192.168.1/24;
RETURN 192.168.1/24 <<= 192.168.1/24;

--
-- inet >> inet
--
RETURN 192.168.1.5 >> 192.168.1/24;
RETURN 192.168.0.5 >> 192.168.1/24;
RETURN 192.168.1/24 >> 192.168.1/24;

--
-- inet >>= inet
--
RETURN 192.168.1.5 >>= 192.168.1/24;
RETURN 192.168.0.5 >>= 192.168.1/24;
RETURN 192.168.1/24 >>= 192.168.1/24;

--
-- inet && inet
--
RETURN 192.168.1.5 && 192.168.1/24;
RETURN 192.168.0.5 && 192.168.1/24;
RETURN 192.168.1/24 && 192.168.1/24;

--
-- abbrev
--
RETURN abbrev(192.168.1.5);
RETURN abbrev(192.168.1/24);
RETURN abbrev('::ffff:fff0:1'::inet);
RETURN abbrev(10.1.0.0/32);

RETURN abbrev('192.168.1.5'::cidr);
RETURN abbrev('192.168.1/24'::cidr);
RETURN abbrev('::ffff:fff0:1'::cidr);
RETURN abbrev('10.1.0.0/32'::cidr);
RETURN abbrev('10.1.0.0/16'::cidr);

--
-- broadcast
--
RETURN broadcast(192.168.1.5);
RETURN broadcast(192.168.1/24);
RETURN broadcast('::ffff:fff0:1'::inet);
RETURN broadcast(10.1.0.0/32);

--
-- family
--
RETURN family(192.168.1.5);
RETURN family(192.168.1/24);
RETURN family('::ffff:fff0:1'::inet);
RETURN family(10.1.0.0/32);

--
-- host
--
RETURN host(192.168.1.5);
RETURN host(192.168.1/24);
RETURN host('::ffff:fff0:1'::inet);
RETURN host(10.1.0.0/32);

--
-- hostmask
--
RETURN hostmask(192.168.1.5);
RETURN hostmask(192.168.1/24);
RETURN hostmask('::ffff:fff0:1'::inet);
RETURN hostmask(10.1.0.0/32);

--
-- inet_merge
--
RETURN inet_merge(192.168.1.5/24, 192.168.2.5/24);

--
-- inet_same_family
--
RETURN inet_same_family(192.168.1.5/24, 192.168.2.5/24);
RETURN inet_same_family(192.168.1.5/24, '::1'::inet);

--
-- masklen
--
RETURN masklen(192.168.1.5);
RETURN masklen(192.168.1/24);
RETURN masklen('::ffff:fff0:1'::inet);
RETURN masklen(10.1.0.0/32);

--
-- netmask
--
RETURN netmask(192.168.1.5);
RETURN netmask(192.168.1/24);
RETURN netmask('::ffff:fff0:1'::inet);
RETURN netmask(10.1.0.0/32);
RETURN netmask(192.168.1/8);

--
-- network
--
RETURN network(192.168.1.5);
RETURN network(192.168.1/24);
RETURN network('::ffff:fff0:1'::inet);
RETURN network(10.1.0.0/32);
RETURN network(192.168.1/8);


--
-- set_masklen
--
RETURN set_masklen(192.168.1.5, 24);
RETURN set_masklen(192.168.1/24, 16);
RETURN set_masklen('::ffff:fff0:1'::inet, 24);
RETURN set_masklen(10.1.0.0/32, 8);
RETURN set_masklen(192.168.1/8, 24);

RETURN set_masklen('192.168.1.5'::cidr, 24);
RETURN set_masklen('192.168.1/24'::cidr, 16);
RETURN set_masklen('::ffff:fff0:1'::cidr, 24);
RETURN set_masklen('10.1.0.0/32'::cidr, 8);
RETURN set_masklen('192.168.1/8'::cidr, 24);
RETURN set_masklen('192.168.1.0/24'::cidr, 16);
RETURN set_masklen('10.1.0.0/32'::cidr, 4);

--
-- abbrev
--
RETURN trunc('12:34:56:78:90:ab'::macaddr);

RETURN trunc('12:34:56:78:90:ab:cd:ef'::macaddr8);

--
-- macaddr8_set7bit
--
RETURN macaddr8_set7bit('12:34:56:78:90:ab:cd:ef'::macaddr8);
RETURN macaddr8_set7bit('00:34:56:ab:cd:ef'::macaddr8);

--
-- Cleanup
--
DROP GRAPH network CASCADE;
