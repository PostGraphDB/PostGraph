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
 *
 */

#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "portability/instr_time.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/rangetypes.h"
#include "utils/geo_decls.h"
#include "common/int128.h"

#include "utils/gtype.h"
#include "utils/edge.h"
#include "utils/variable_edge.h"
#include "utils/vector.h"
#include "utils/vertex.h"
#include "utils/gtype_parser.h"
#include "utils/gtype_typecasting.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "utils/graphid.h"
#include "utils/numeric.h"



#define GTYPETRIGFUNC( name)                                                               \
PG_FUNCTION_INFO_V1(gtype_##name);                                                         \
Datum                                                                                      \
gtype_##name(PG_FUNCTION_ARGS)                                                             \
{                                                                                          \
    gtype *gt= AG_GET_ARG_GTYPE_P(0);                                                      \
    gtype_value gtv = { \
        .type =AGTV_FLOAT, \
	.val.float_value = DatumGetFloat8(DirectFunctionCall1(d##name, GT_ARG_TO_FLOAT8_DATUM(0))) \
    }; \
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));                                         \
}                                                                                          \
/* keep compiler quiet - no extra ; */                                                     \
extern int no_such_variable

GTYPETRIGFUNC(sin);
GTYPETRIGFUNC(cos);
GTYPETRIGFUNC(tan);
GTYPETRIGFUNC(sinh);
GTYPETRIGFUNC(cosh);
GTYPETRIGFUNC(tanh);
GTYPETRIGFUNC(cot);
GTYPETRIGFUNC(asin);
GTYPETRIGFUNC(acos);
GTYPETRIGFUNC(atan);
GTYPETRIGFUNC(asinh);
GTYPETRIGFUNC(acosh);
GTYPETRIGFUNC(atanh);

GTYPETRIGFUNC(sind);
GTYPETRIGFUNC(cosd);
GTYPETRIGFUNC(tand);
GTYPETRIGFUNC(cotd);
GTYPETRIGFUNC(asind);
GTYPETRIGFUNC(acosd);
GTYPETRIGFUNC(atand);


PG_FUNCTION_INFO_V1(gtype_atan2);
Datum
gtype_atan2(PG_FUNCTION_ARGS) {
    Datum x = convert_to_scalar(gtype_to_float8_internal, AG_GET_ARG_GTYPE_P(0), "float");
    Datum y = convert_to_scalar(gtype_to_float8_internal, AG_GET_ARG_GTYPE_P(1), "float");

    gtype_value gtv_result;
    gtv_result.type = AGTV_FLOAT;
    gtv_result.val.float_value = DatumGetFloat8(DirectFunctionCall2(datan2, y, x));

    PG_RETURN_POINTER(gtype_value_to_gtype(&gtv_result));
}

PG_FUNCTION_INFO_V1(gtype_degrees);

Datum gtype_degrees(PG_FUNCTION_ARGS)
{
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    gtype_value gtv_result;
    gtv_result.type = AGTV_FLOAT;
    gtv_result.val.float_value =
        DatumGetFloat8(DirectFunctionCall1(degrees, convert_to_scalar(gtype_to_float8_internal, gt, "float")));

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv_result));
}

PG_FUNCTION_INFO_V1(gtype_radians);

Datum gtype_radians(PG_FUNCTION_ARGS)
{
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    gtype_value gtv_result;
    gtv_result.type = AGTV_FLOAT;
    gtv_result.val.float_value =
        DatumGetFloat8(DirectFunctionCall1(radians, convert_to_scalar(gtype_to_float8_internal, gt, "float")));

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv_result));
}

PG_FUNCTION_INFO_V1(gtype_gcd);
Datum
gtype_gcd(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (GTYPE_IS_FLOAT(lhs) || GTE_IS_NUMERIC(lhs->root.children[0]) || GTYPE_IS_FLOAT(rhs) || GTE_IS_NUMERIC(rhs->root.children[0])) {
        Datum x = convert_to_scalar(gtype_to_numeric_internal, lhs, "numeric");
        Datum y = convert_to_scalar(gtype_to_numeric_internal, rhs, "numeric");

        gtype_value gtv_result;
        gtv_result.type = AGTV_NUMERIC;
        gtv_result.val.numeric = DatumGetNumeric(DirectFunctionCall2(numeric_gcd, y, x));

        PG_RETURN_POINTER(gtype_value_to_gtype(&gtv_result));
    } else {
        Datum x = convert_to_scalar(gtype_to_int8_internal, lhs, "int8");
        Datum y = convert_to_scalar(gtype_to_int8_internal, rhs, "int8");

        gtype_value gtv_result;
        gtv_result.type = AGTV_INTEGER;
        gtv_result.val.int_value = DatumGetInt64(DirectFunctionCall2(int8gcd, y, x));

        PG_RETURN_POINTER(gtype_value_to_gtype(&gtv_result));
    }
}

PG_FUNCTION_INFO_V1(gtype_lcm);
Datum
gtype_lcm(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (GTYPE_IS_FLOAT(lhs) || GTE_IS_NUMERIC(lhs->root.children[0]) || GTYPE_IS_FLOAT(rhs) || GTE_IS_NUMERIC(rhs->root.children[0])) {
        Datum x = convert_to_scalar(gtype_to_numeric_internal, lhs, "numeric");
        Datum y = convert_to_scalar(gtype_to_numeric_internal, rhs, "numeric");

        gtype_value gtv_result;
        gtv_result.type = AGTV_NUMERIC;
        gtv_result.val.numeric = DatumGetNumeric(DirectFunctionCall2(numeric_lcm, y, x));

        PG_RETURN_POINTER(gtype_value_to_gtype(&gtv_result));
    } else {
        Datum x = convert_to_scalar(gtype_to_int8_internal, lhs, "int8");
        Datum y = convert_to_scalar(gtype_to_int8_internal, rhs, "int8");

        gtype_value gtv_result;
        gtv_result.type = AGTV_INTEGER;
        gtv_result.val.int_value = DatumGetInt64(DirectFunctionCall2(int8lcm, y, x));

        PG_RETURN_POINTER(gtype_value_to_gtype(&gtv_result));
    }
}

PG_FUNCTION_INFO_V1(gtype_round_w_precision);

Datum gtype_round_w_precision(PG_FUNCTION_ARGS)
{
     gtype *gt = AG_GET_ARG_GTYPE_P(0);
     gtype *prec = AG_GET_ARG_GTYPE_P(1);

     Datum x = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");
     Datum y = convert_to_scalar(gtype_to_int4_internal, prec, "int4");

     gtype_value gtv;
     gtv.type = AGTV_NUMERIC;
     gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(numeric_round, x, y));

     AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_round);

Datum gtype_round(PG_FUNCTION_ARGS)
{
     gtype *gt = AG_GET_ARG_GTYPE_P(0);

     if (is_gtype_numeric(gt)) {
         Datum x = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");

         gtype_value gtv;
         gtv.type = AGTV_NUMERIC;
         gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(numeric_round, x, Int32GetDatum(0)));

         AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
     } else {
         Datum x = convert_to_scalar(gtype_to_float8_internal, gt, "float");

         gtype_value gtv;
         gtv.type = AGTV_FLOAT;
         gtv.val.float_value = DatumGetFloat8(DirectFunctionCall1(dround, x));

         AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
     }
}

PG_FUNCTION_INFO_V1(gtype_ceil);

Datum gtype_ceil(PG_FUNCTION_ARGS)
{
    gtype *gt = AG_GET_ARG_GTYPE_P(0);
    
    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");
    
        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_ceil, arg));
    
        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };
            
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");
    
        float8 result = DatumGetFloat8(DirectFunctionCall1(dceil, arg));
        
        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }
}

PG_FUNCTION_INFO_V1(gtype_floor);

Datum gtype_floor(PG_FUNCTION_ARGS)
{
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");

        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_floor, arg));
        
        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };
            
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");

        float8 result = DatumGetFloat8(DirectFunctionCall1(dfloor, arg));
        
        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };
        
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }
}

PG_FUNCTION_INFO_V1(gtype_abs);

Datum gtype_abs(PG_FUNCTION_ARGS)
{
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum x;
    gtype_value gtv;
    if (is_gtype_numeric(gt)) {
        gtv.type = AGTV_NUMERIC;
        x = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");
        gtv.val.numeric = DatumGetNumeric(DirectFunctionCall1(numeric_abs, x));
    }
    else if (is_gtype_float(gt)) {
        gtv.type = AGTV_FLOAT;
        x = convert_to_scalar(gtype_to_float8_internal, gt, "float");
        gtv.val.float_value = DatumGetFloat8(DirectFunctionCall1(float8abs, x));
    }
    else {
        gtv.type = AGTV_INTEGER;
        x = convert_to_scalar(gtype_to_int8_internal, gt, "int");
        gtv.val.int_value = DatumGetInt64(DirectFunctionCall1(int8abs, x));
    }

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_sign);
Datum gtype_sign(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum x;
    gtype_value gtv;
    if (is_gtype_numeric(gt)) {
        gtv.type = AGTV_NUMERIC;
        x = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");
        gtv.val.numeric = DatumGetNumeric(DirectFunctionCall1(numeric_sign, x));
    } else {
        gtv.type = AGTV_FLOAT;
        x = convert_to_scalar(gtype_to_float8_internal, gt, "float");
        gtv.val.float_value = DatumGetFloat8(DirectFunctionCall1(dsign, x));
    }

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_log);
Datum
gtype_log(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);
    
    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");
        
        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_ln, arg));
        
        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };
            
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");
        
        float8 result = DatumGetFloat8(DirectFunctionCall1(dlog1, arg));
        
        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };
 
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }
}

PG_FUNCTION_INFO_V1(gtype_log10);
Datum
gtype_log10(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");

        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_log, arg));

        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");

        float8 result = DatumGetFloat8(DirectFunctionCall1(dlog10, arg));

        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }
}

PG_FUNCTION_INFO_V1(gtype_e);
Datum
gtype_e(PG_FUNCTION_ARGS) {
    gtype_value agtv_result = {
        .type = AGTV_FLOAT,
        .val.float_value = DatumGetFloat8(DirectFunctionCall1(dexp, Float8GetDatum(1)))
    };

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(gtype_pi);
Datum
gtype_pi(PG_FUNCTION_ARGS) {   
    gtype_value agtv_result = {
        .type = AGTV_FLOAT,
        .val.float_value = M_PI
    };

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}   

PG_FUNCTION_INFO_V1(gtype_rand);
Datum
gtype_rand(PG_FUNCTION_ARGS) {
    gtype_value agtv_result = {
        .type = AGTV_FLOAT,
        .val.float_value = DatumGetFloat8(DirectFunctionCall1(random, Float8GetDatum(1)))
    };

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(gtype_exp);
Datum
gtype_exp(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");

        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_exp, arg));

        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");
        
        float8 result = DatumGetFloat8(DirectFunctionCall1(dexp, arg));
        
        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }

}

PG_FUNCTION_INFO_V1(gtype_sqrt);
Datum
gtype_sqrt(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_numeric(gt)) {
        Datum arg = convert_to_scalar(gtype_to_numeric_internal, gt, "numeric");

        Numeric result = DatumGetNumeric(DirectFunctionCall1(numeric_sqrt, arg));

        gtype_value agtv = { .type = AGTV_NUMERIC, .val.numeric = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    } else {
        Datum arg = convert_to_scalar(gtype_to_float8_internal, gt, "float");

        float8 result = DatumGetFloat8(DirectFunctionCall1(dsqrt, arg));                                                       

        gtype_value agtv = { .type = AGTV_FLOAT, .val.float_value = result };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&agtv));
    }
}

PG_FUNCTION_INFO_V1(gtype_cbrt);
Datum
gtype_cbrt(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    gtype_value gtv_result;
    gtv_result.type = AGTV_FLOAT;
    gtv_result.val.float_value =
        DatumGetFloat8(DirectFunctionCall1(dcbrt, convert_to_scalar(gtype_to_float8_internal, gt, "float")));

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv_result));
}

PG_FUNCTION_INFO_V1(gtype_factorial);
Datum
gtype_factorial(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    gtype_value gtv_result;
    gtv_result.type = AGTV_NUMERIC;
    gtv_result.val.numeric =
        DatumGetNumeric(DirectFunctionCall1(numeric_fac, convert_to_scalar(gtype_to_int8_internal, gt, "int")));

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv_result));
}


PG_FUNCTION_INFO_V1(gtype_abbrev);
Datum
gtype_abbrev(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum d;
    if (GT_IS_CIDR(gt))
        d = DirectFunctionCall1(cidr_abbrev, GT_TO_CIDR_DATUM(gt));
    else 
        d = DirectFunctionCall1(inet_abbrev, GT_TO_INET_DATUM(gt));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_broadcast);
Datum
gtype_broadcast(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_broadcast, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv;
    gtv.type = AGTV_INET;
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_family);
Datum
gtype_family(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_family, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv = { .type = AGTV_INTEGER, .val.int_value = DatumGetInt32(d) };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
  
}

PG_FUNCTION_INFO_V1(gtype_host);
Datum
gtype_host(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_host, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_hostmask);
Datum
gtype_hostmask(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_hostmask, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv;
    gtv.type = AGTV_INET;
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_inet_merge);
Datum
gtype_inet_merge(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall2(inet_merge, GT_ARG_TO_INET_DATUM(0), GT_ARG_TO_INET_DATUM(1));

    gtype_value gtv;
    gtv.type = AGTV_CIDR;
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_inet_same_family);
Datum
gtype_inet_same_family(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall2(inet_same_family, GT_ARG_TO_INET_DATUM(0), GT_ARG_TO_INET_DATUM(1));

    gtype_value gtv = { .type = AGTV_BOOL, .val.int_value = DatumGetBool(d) };
    
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_masklen);
Datum
gtype_masklen(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_masklen, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv = { .type = AGTV_INTEGER, .val.int_value = DatumGetInt32(d) };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_netmask);
Datum
gtype_netmask(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_netmask, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv;
    gtv.type = AGTV_INET;
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_network);
Datum
gtype_network(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(network_network, GT_ARG_TO_INET_DATUM(0));

    gtype_value gtv;
    gtv.type = AGTV_CIDR;
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_set_masklen);
Datum
gtype_set_masklen(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);
    gtype_value gtv;
    Datum d;
    if (GT_IS_CIDR(gt)) {
        d = DirectFunctionCall2(cidr_set_masklen, GT_TO_CIDR_DATUM(gt), GT_ARG_TO_INT4_DATUM(1));
        gtv.type = AGTV_CIDR;
    } else {
        d = DirectFunctionCall2(inet_set_masklen, GT_TO_INET_DATUM(gt), GT_ARG_TO_INT4_DATUM(1));
       gtv.type = AGTV_INET;
    }
    
    memcpy(&gtv.val.inet, DatumGetInetPP(d), sizeof(char) * 22);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_trunc);
Datum
gtype_trunc(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    gtype_value gtv;
    Datum d;
    if (GT_IS_MACADDR(gt)) {
        d = DirectFunctionCall1(macaddr_trunc, GT_TO_MAC_DATUM(gt));
        gtv.type = AGTV_MAC;
    } else {
        d = DirectFunctionCall1(macaddr8_trunc, GT_TO_MAC8_DATUM(gt));
       gtv.type = AGTV_MAC8;
    }
    
    memcpy(&gtv.val.inet, DatumGetMacaddrP(d), sizeof(char) * 6);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_macaddr8_set7bit);
Datum
gtype_macaddr8_set7bit(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(macaddr8_set7bit, GT_ARG_TO_MAC8_DATUM(0));

    gtype_value gtv;
    gtv.type = AGTV_MAC8;
    memcpy(&gtv.val.inet, DatumGetMacaddr8P(d), sizeof(char) * 8);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}



int
timetz_cmp_internal(TimeTzADT *time1, TimeTzADT *time2) {                                
    TimeOffset t1, t2;

    // Primary sort is by true (GMT-equivalent) time
    t1 = time1->time + (time1->zone * USECS_PER_SEC);
    t2 = time2->time + (time2->zone * USECS_PER_SEC);

    if (t1 > t2)
        return 1;
    if (t1 < t2)
        return -1;

    /*
     * If same GMT time, sort by timezone; we only want to say that two
     * timetz's are equal if both the time and zone parts are equal.
     */
    if (time1->zone > time2->zone)
        return 1;
    if (time1->zone < time2->zone)
        return -1;

    return 0;
}


static inline INT128
interval_cmp_value(const Interval *interval)
{
    INT128 span;
    int64 dayfraction;
    int64 days;

    /*
     * Separate time field into days and dayfraction, then add the month and
     * day fields to the days part.  We cannot overflow int64 days here.
     */
    dayfraction = interval->time % USECS_PER_DAY;
    days = interval->time / USECS_PER_DAY;
    days += interval->month * INT64CONST(30);
    days += interval->day;
        
    // Widen dayfraction to 128 bits
    span = int64_to_int128(dayfraction);

    // Scale up days to microseconds, forming a 128-bit product
    int128_add_int64_mul_int64(&span, days, USECS_PER_DAY);

    return span;
}

int
interval_cmp_internal(Interval *interval1, Interval *interval2)
{
    INT128 span1 = interval_cmp_value(interval1);
    INT128 span2 = interval_cmp_value(interval2);
 
    return int128_compare(span1, span2);
}

PG_FUNCTION_INFO_V1(gtype_age_today);
Datum gtype_age_today(PG_FUNCTION_ARGS)
{
    Timestamp ts;
    gtype *arg1 = AG_GET_ARG_GTYPE_P(0);
    gtype_value agtv_result, *agtv1;
    Interval *i;

    if (is_gtype_null(arg1))
        PG_RETURN_NULL();
    agtv1 = get_ith_gtype_value_from_container(&arg1->root, 0);

    ts = TimestampGetDatum(GetCurrentTransactionStartTimestamp());
    ts = DatumGetTimestamp(DirectFunctionCall2(timestamp_trunc, cstring_to_text_with_len("day",3), ts));

    if (agtv1->type != AGTV_TIMESTAMP)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("age(gtype) only supports timestamps")));

    i = DatumGetIntervalP(DirectFunctionCall2(timestamp_mi,
                                              TimestampGetDatum(agtv1->val.int_value),
                                              ts));

    agtv_result.type = AGTV_INTERVAL;
    agtv_result.val.interval.time = i->time;
    agtv_result.val.interval.day = i->day;
    agtv_result.val.interval.month = i->month;

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(gtype_age_w2args);
Datum
gtype_age_w2args(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (is_gtype_null(lhs) || is_gtype_null(rhs))
        PG_RETURN_NULL();

    gtype_value *lhs_value = get_ith_gtype_value_from_container(&lhs->root, 0);
    gtype_value *rhs_value = get_ith_gtype_value_from_container(&rhs->root, 0); 

    Interval *i;
    if (lhs_value->type == AGTV_TIMESTAMP && rhs_value->type == AGTV_TIMESTAMP)
         i = DatumGetIntervalP(DirectFunctionCall2(timestamp_age,
				    TimestampGetDatum(lhs_value->val.int_value),
				    TimestampGetDatum(rhs_value->val.int_value)));
    else if (lhs_value->type == AGTV_TIMESTAMPTZ && rhs_value->type == AGTV_TIMESTAMPTZ)
         i = DatumGetIntervalP(DirectFunctionCall2(timestamptz_age,
                                    TimestampTzGetDatum(lhs_value->val.int_value),
                                    TimestampTzGetDatum(rhs_value->val.int_value)));
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for age(gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));

    gtype_value gtv;
    gtv.type = AGTV_INTERVAL;
    gtv.val.interval.time = i->time;
    gtv.val.interval.day = i->day;
    gtv.val.interval.month = i->month;

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_extract);
Datum
gtype_extract(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (is_gtype_null(lhs) || is_gtype_null(rhs))
        PG_RETURN_NULL();

    gtype_value *lhs_value = get_ith_gtype_value_from_container(&lhs->root, 0);
    gtype_value *rhs_value = get_ith_gtype_value_from_container(&rhs->root, 0);

    if (lhs_value->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for EXTRACT arg 1 must be a string"),
                        errhint("You may have to use explicit casts.")));

    char *field = lhs_value->val.string.val;
    
    gtype_value gtv;
    if (rhs_value->type == AGTV_TIMESTAMP) {
        gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_timestamp,
                                               CStringGetTextDatum(field),
                                               TimestampGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_TIMESTAMPTZ) {
        gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_timestamptz,
                                              CStringGetTextDatum(field),
                                              TimestampTzGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_DATE) {
         gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_date,
                                              CStringGetTextDatum(field),
                                              DateADTGetDatum(rhs_value->val.date)));
    } else if (rhs_value->type == AGTV_TIME) {
         gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_time,
                                              CStringGetTextDatum(field),
                                              TimeADTGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_TIMETZ) {
         gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_timetz,
                                              CStringGetTextDatum(field),
                                              TimeTzADTPGetDatum(&rhs_value->val.timetz)));
    } else if (rhs_value->type == AGTV_INTERVAL) {
         gtv.val.numeric = DatumGetNumeric(DirectFunctionCall2(extract_interval,
                                              CStringGetTextDatum(field),
                                              IntervalPGetDatum(&rhs_value->val.interval)));
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for EXTRACT(gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));
    }

    gtv.type = AGTV_NUMERIC;

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_date_part);
Datum
gtype_date_part(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (is_gtype_null(lhs) || is_gtype_null(rhs))
        PG_RETURN_NULL();

    gtype_value *lhs_value = get_ith_gtype_value_from_container(&lhs->root, 0);
    gtype_value *rhs_value = get_ith_gtype_value_from_container(&rhs->root, 0);

    if (lhs_value->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for EXTRACT arg 1 must be a string"),
                        errhint("You may have to use explicit casts.")));

    char *field = lhs_value->val.string.val;

    gtype_value gtv;
    if (rhs_value->type == AGTV_TIMESTAMP) {
        gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(timestamp_part,
                                               CStringGetTextDatum(field),
                                               TimestampGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_TIMESTAMPTZ) {
        gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(timestamptz_part,
                                              CStringGetTextDatum(field),
                                              TimestampTzGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_DATE) {
	Datum ts = gtype_to_timestamptz_internal(rhs_value);
        gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(timestamptz_part,
                                              CStringGetTextDatum(field), ts));
    } else if (rhs_value->type == AGTV_TIME) {
         gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(time_part,
                                              CStringGetTextDatum(field),
                                              TimeADTGetDatum(rhs_value->val.int_value)));
    } else if (rhs_value->type == AGTV_TIMETZ) {
         gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(timetz_part,
                                              CStringGetTextDatum(field),
                                              TimeTzADTPGetDatum(&rhs_value->val.timetz)));
    } else if (rhs_value->type == AGTV_INTERVAL) {
         gtv.val.float_value = DatumGetFloat8(DirectFunctionCall2(interval_part,
                                              CStringGetTextDatum(field),
                                              IntervalPGetDatum(&rhs_value->val.interval)));
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_part(gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));
    }

    gtv.type = AGTV_FLOAT;

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_make_date);

Datum gtype_make_date(PG_FUNCTION_ARGS)
{
    gtype_value agtv, *agtv_year, *agtv_month, *agtv_day;
    gtype *agt_year = AG_GET_ARG_GTYPE_P(0);
    gtype *agt_month = AG_GET_ARG_GTYPE_P(1);
    gtype *agt_day = AG_GET_ARG_GTYPE_P(2);

    if (is_gtype_null(agt_year) || is_gtype_null(agt_month) || is_gtype_null(agt_day))
        PG_RETURN_NULL();

    agtv_year = get_ith_gtype_value_from_container(&agt_year->root, 0);
    agtv_month = get_ith_gtype_value_from_container(&agt_month->root, 0);
    agtv_day = get_ith_gtype_value_from_container(&agt_day->root, 0);


    if (agtv_year->type != AGTV_INTEGER || agtv_month->type != AGTV_INTEGER || agtv_day->type != AGTV_INTEGER)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_date(gtype, gtype, gtype) only supports integer arugments")));


    agtv.type = AGTV_DATE;
    agtv.val.date = DatumGetDateADT(DirectFunctionCall3(make_date,
                                                               Int32GetDatum((int32)agtv_year->val.int_value),
                                                               Int32GetDatum((int32)agtv_month->val.int_value),
                                                               Int32GetDatum((int32)agtv_day->val.int_value)));

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv));
}

PG_FUNCTION_INFO_V1(gtype_make_time);

Datum gtype_make_time(PG_FUNCTION_ARGS)
{
    gtype_value agtv, *agtv_hour, *agtv_minute, *agtv_second;
    gtype *agt_hour = AG_GET_ARG_GTYPE_P(0);
    gtype *agt_minute = AG_GET_ARG_GTYPE_P(1);
    gtype *agt_second = AG_GET_ARG_GTYPE_P(2);

    if (is_gtype_null(agt_hour) || is_gtype_null(agt_minute) || is_gtype_null(agt_second))
        PG_RETURN_NULL();

    agtv_hour = get_ith_gtype_value_from_container(&agt_hour->root, 0);
    agtv_minute = get_ith_gtype_value_from_container(&agt_minute->root, 0);
    agtv_second = get_ith_gtype_value_from_container(&agt_second->root, 0);


    if (agtv_hour->type != AGTV_INTEGER || agtv_minute->type != AGTV_INTEGER || agtv_second->type != AGTV_FLOAT)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_time(gtype, gtype, gtype) only supports integer arugments")));


    agtv.type = AGTV_TIME;
    agtv.val.int_value = DatumGetTimeADT(DirectFunctionCall3(make_time,
                                                          Int32GetDatum((int32)agtv_hour->val.int_value),
                                                          Int32GetDatum((int32)agtv_minute->val.int_value),
                                                          Float8GetDatum(agtv_second->val.float_value)));

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv));
}

PG_FUNCTION_INFO_V1(gtype_make_timestamp);

Datum gtype_make_timestamp(PG_FUNCTION_ARGS)
{
    gtype_value agtv;
    gtype_value *agtv_hour, *agtv_minute, *agtv_second;
    gtype_value *agtv_year, *agtv_month, *agtv_day;

    gtype *agt_year = AG_GET_ARG_GTYPE_P(0);
    gtype *agt_month = AG_GET_ARG_GTYPE_P(1);
    gtype *agt_day = AG_GET_ARG_GTYPE_P(2);
    gtype *agt_hour = AG_GET_ARG_GTYPE_P(3);
    gtype *agt_minute = AG_GET_ARG_GTYPE_P(4);
    gtype *agt_second = AG_GET_ARG_GTYPE_P(5);

    if (is_gtype_null(agt_year) || is_gtype_null(agt_month) || is_gtype_null(agt_day) ||
        is_gtype_null(agt_hour) || is_gtype_null(agt_minute) || is_gtype_null(agt_second))
        PG_RETURN_NULL();

    agtv_year = get_ith_gtype_value_from_container(&agt_year->root, 0);
    agtv_month = get_ith_gtype_value_from_container(&agt_month->root, 0);
    agtv_day = get_ith_gtype_value_from_container(&agt_day->root, 0);
    agtv_hour = get_ith_gtype_value_from_container(&agt_hour->root, 0);
    agtv_minute = get_ith_gtype_value_from_container(&agt_minute->root, 0);
    agtv_second = get_ith_gtype_value_from_container(&agt_second->root, 0);

    if (agtv_year->type != AGTV_INTEGER || agtv_month->type != AGTV_INTEGER || agtv_day->type != AGTV_INTEGER ||
        agtv_hour->type != AGTV_INTEGER || agtv_minute->type != AGTV_INTEGER || agtv_second->type != AGTV_FLOAT)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_time(gtype, gtype, gtype) only supports integer arugments")));


    agtv.type = AGTV_TIMESTAMP;
    agtv.val.int_value = DatumGetTimestamp(DirectFunctionCall6(make_timestamp,
                                                               Int32GetDatum((int32)agtv_year->val.int_value),
                                                               Int32GetDatum((int32)agtv_month->val.int_value),
                                                               Int32GetDatum((int32)agtv_day->val.int_value),
                                                               Int32GetDatum((int32)agtv_hour->val.int_value),
                                                               Int32GetDatum((int32)agtv_minute->val.int_value),
                                                               Float8GetDatum(agtv_second->val.float_value)));

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv));
}

PG_FUNCTION_INFO_V1(gtype_make_timestamptz);

Datum gtype_make_timestamptz(PG_FUNCTION_ARGS)
{
    gtype_value agtv;
    gtype_value *agtv_hour, *agtv_minute, *agtv_second;
    gtype_value *agtv_year, *agtv_month, *agtv_day;

    gtype *agt_year = AG_GET_ARG_GTYPE_P(0);
    gtype *agt_month = AG_GET_ARG_GTYPE_P(1);
    gtype *agt_day = AG_GET_ARG_GTYPE_P(2);
    gtype *agt_hour = AG_GET_ARG_GTYPE_P(3);
    gtype *agt_minute = AG_GET_ARG_GTYPE_P(4);
    gtype *agt_second = AG_GET_ARG_GTYPE_P(5);

    if (is_gtype_null(agt_year) || is_gtype_null(agt_month) || is_gtype_null(agt_day) ||
        is_gtype_null(agt_hour) || is_gtype_null(agt_minute) || is_gtype_null(agt_second))
        PG_RETURN_NULL();

    agtv_year = get_ith_gtype_value_from_container(&agt_year->root, 0);
    agtv_month = get_ith_gtype_value_from_container(&agt_month->root, 0);
    agtv_day = get_ith_gtype_value_from_container(&agt_day->root, 0);
    agtv_hour = get_ith_gtype_value_from_container(&agt_hour->root, 0);
    agtv_minute = get_ith_gtype_value_from_container(&agt_minute->root, 0);
    agtv_second = get_ith_gtype_value_from_container(&agt_second->root, 0);

    if (agtv_year->type != AGTV_INTEGER || agtv_month->type != AGTV_INTEGER || agtv_day->type != AGTV_INTEGER ||
        agtv_hour->type != AGTV_INTEGER || agtv_minute->type != AGTV_INTEGER)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_timestamptz expected an integer arugment")));


    if (agtv_second->type != AGTV_FLOAT)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_timestamptz second arguement expects a float")));

    agtv.type = AGTV_TIMESTAMPTZ;
    agtv.val.int_value = DatumGetTimestampTz(DirectFunctionCall6(make_timestamp,
                                                               Int32GetDatum((int32)agtv_year->val.int_value),
                                                               Int32GetDatum((int32)agtv_month->val.int_value),
                                                               Int32GetDatum((int32)agtv_day->val.int_value),
                                                               Int32GetDatum((int32)agtv_hour->val.int_value),
                                                               Int32GetDatum((int32)agtv_minute->val.int_value),
                                                               Float8GetDatum(agtv_second->val.float_value)));

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv));
}

PG_FUNCTION_INFO_V1(gtype_make_timestamptz_wtimezone);

Datum gtype_make_timestamptz_wtimezone(PG_FUNCTION_ARGS)
{
    gtype_value agtv;
    gtype_value *agtv_hour, *agtv_minute, *agtv_second;
    gtype_value *agtv_year, *agtv_month, *agtv_day;
    gtype_value *agtv_timezone;
    gtype *agt_year = AG_GET_ARG_GTYPE_P(0);
    gtype *agt_month = AG_GET_ARG_GTYPE_P(1);
    gtype *agt_day = AG_GET_ARG_GTYPE_P(2);
    gtype *agt_hour = AG_GET_ARG_GTYPE_P(3);
    gtype *agt_minute = AG_GET_ARG_GTYPE_P(4);
    gtype *agt_second = AG_GET_ARG_GTYPE_P(5);
    gtype *agt_timezone = AG_GET_ARG_GTYPE_P(6);

    if (is_gtype_null(agt_year) || is_gtype_null(agt_month) || is_gtype_null(agt_day) ||
        is_gtype_null(agt_hour) || is_gtype_null(agt_minute) || is_gtype_null(agt_second) ||
        is_gtype_null(agt_timezone))
        PG_RETURN_NULL();

    agtv_year = get_ith_gtype_value_from_container(&agt_year->root, 0);
    agtv_month = get_ith_gtype_value_from_container(&agt_month->root, 0);
    agtv_day = get_ith_gtype_value_from_container(&agt_day->root, 0);
    agtv_hour = get_ith_gtype_value_from_container(&agt_hour->root, 0);
    agtv_minute = get_ith_gtype_value_from_container(&agt_minute->root, 0);
    agtv_second = get_ith_gtype_value_from_container(&agt_second->root, 0);
    agtv_timezone = get_ith_gtype_value_from_container(&agt_timezone->root, 0);

    if (agtv_year->type != AGTV_INTEGER || agtv_month->type != AGTV_INTEGER || agtv_day->type != AGTV_INTEGER ||
        agtv_hour->type != AGTV_INTEGER || agtv_minute->type != AGTV_INTEGER)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_timestamptz expected an integer arugment")));


    if (agtv_second->type != AGTV_FLOAT)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_timestamptz second arguement expects a float")));

    if (agtv_timezone->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("make_timestamptz timezone arguemnent must be a string")));


    agtv.type = AGTV_TIMESTAMPTZ;
    agtv.val.int_value = DatumGetTimestampTz(DirectFunctionCall7(make_timestamptz_at_timezone,
                                                               Int32GetDatum((int32)agtv_year->val.int_value),
                                                               Int32GetDatum((int32)agtv_month->val.int_value),
                                                               Int32GetDatum((int32)agtv_day->val.int_value),
                                                               Int32GetDatum((int32)agtv_hour->val.int_value),
                                                               Int32GetDatum((int32)agtv_minute->val.int_value),
                                                               Float8GetDatum(agtv_second->val.float_value),
                                                               PointerGetDatum(cstring_to_text_with_len(agtv_timezone->val.string.val,
                                                                                                        agtv_timezone->val.string.len))));

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv));
}

PG_FUNCTION_INFO_V1(gtype_isfinite);
Datum gtype_isfinite(PG_FUNCTION_ARGS)
{
    gtype *agt = AG_GET_ARG_GTYPE_P(0);
    gtype_value *agtv, agtv_result;
    bool result = false;

    if (is_gtype_null(agt))
        PG_RETURN_NULL();

    agtv = get_ith_gtype_value_from_container(&agt->root, 0);

    if (agtv->type == AGTV_DATE)
        result = DatumGetBool(DirectFunctionCall1(date_finite, DateADTGetDatum(agtv->val.date)));
    else if (agtv->type == AGTV_INTERVAL)
        result = true;
    else if(agtv->type == AGTV_TIMESTAMP || agtv->type == AGTV_TIMESTAMPTZ)
    {
        result = DatumGetBool(DirectFunctionCall1(timestamp_finite, TimestampGetDatum(agtv->val.int_value)));
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for isfinite(gtype)"),
                        errhint("You may have to use explicit casts.")));


    agtv_result.type = AGTV_BOOL;
    agtv_result.val.boolean = result;

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(gtype_justify_days);
Datum
gtype_justify_days(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_null(gt))
        PG_RETURN_NULL();

    gtype_value *gt_value = get_ith_gtype_value_from_container(&gt->root, 0);

    gtype_value gtv;
    if (gt_value->type != AGTV_INTERVAL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for justify_days(gtype)"),
                        errhint("You may have to use explicit casts.")));

    Interval *i = DatumGetIntervalP(DirectFunctionCall1(interval_justify_days,
                                              IntervalPGetDatum(&gt_value->val.interval)));
    gtv.type = AGTV_INTERVAL;
    gtv.val.interval.time = i->time;
    gtv.val.interval.day = i->day;
    gtv.val.interval.month = i->month;
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_justify_hours);
Datum
gtype_justify_hours(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_null(gt))
        PG_RETURN_NULL();

    gtype_value *gt_value = get_ith_gtype_value_from_container(&gt->root, 0);

    gtype_value gtv;
    if (gt_value->type != AGTV_INTERVAL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for justify_days(gtype)"),
                        errhint("You may have to use explicit casts.")));

    Interval *i = DatumGetIntervalP(DirectFunctionCall1(interval_justify_hours,
                                              IntervalPGetDatum(&gt_value->val.interval)));
    gtv.type = AGTV_INTERVAL;
    gtv.val.interval.time = i->time;
    gtv.val.interval.day = i->day;
    gtv.val.interval.month = i->month;
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_justify_interval);
Datum
gtype_justify_interval(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    if (is_gtype_null(gt))
        PG_RETURN_NULL();

    gtype_value *gt_value = get_ith_gtype_value_from_container(&gt->root, 0);

    gtype_value gtv;
    if (gt_value->type != AGTV_INTERVAL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for justify_days(gtype)"),
                        errhint("You may have to use explicit casts.")));

    Interval *i = DatumGetIntervalP(DirectFunctionCall1(interval_justify_interval,
                                              IntervalPGetDatum(&gt_value->val.interval)));
    gtv.type = AGTV_INTERVAL;
    gtv.val.interval.time = i->time;
    gtv.val.interval.day = i->day;
    gtv.val.interval.month = i->month;
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));

}

PG_FUNCTION_INFO_V1(gtype_date_trunc);
Datum
gtype_date_trunc(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (is_gtype_null(lhs) || is_gtype_null(rhs))
        PG_RETURN_NULL();

    gtype_value *lhs_value = get_ith_gtype_value_from_container(&lhs->root, 0);
    gtype_value *rhs_value = get_ith_gtype_value_from_container(&rhs->root, 0);

    if (lhs_value->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for EXTRACT arg 1 must be a string"),
                        errhint("You may have to use explicit casts.")));

    char *field = lhs_value->val.string.val;

    gtype_value gtv;
    if (rhs_value->type == AGTV_TIMESTAMP) {
        gtv.val.int_value = DatumGetTimestamp(DirectFunctionCall2(timestamp_trunc,
                                               CStringGetTextDatum(field),
                                               TimestampGetDatum(rhs_value->val.int_value)));
	gtv.type = AGTV_TIMESTAMP;
	AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else if (rhs_value->type == AGTV_TIMESTAMPTZ) {
        gtv.val.int_value = DatumGetTimestampTz(DirectFunctionCall2(timestamptz_trunc,
                                              CStringGetTextDatum(field),
                                              TimestampTzGetDatum(rhs_value->val.int_value)));
        gtv.type = AGTV_TIMESTAMPTZ;
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else if (rhs_value->type == AGTV_INTERVAL) {
         Interval *i = DatumGetIntervalP(DirectFunctionCall2(interval_trunc,
                                              CStringGetTextDatum(field),
                                              IntervalPGetDatum(&rhs_value->val.interval)));
        gtv.type = AGTV_INTERVAL;
        gtv.val.interval.time = i->time;
        gtv.val.interval.day = i->day;
        gtv.val.interval.month = i->month;

	AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_trun(gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));
    }

    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(gtype_date_trunc_zone);
Datum
gtype_date_trunc_zone(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);
    gtype *zone = AG_GET_ARG_GTYPE_P(2);

    if (is_gtype_null(lhs) || is_gtype_null(rhs))
        PG_RETURN_NULL();

    gtype_value *lhs_value = get_ith_gtype_value_from_container(&lhs->root, 0);
    gtype_value *rhs_value = get_ith_gtype_value_from_container(&rhs->root, 0);
    gtype_value *zone_value = get_ith_gtype_value_from_container(&zone->root, 0);

    if (lhs_value->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_trunc arg 1 must be a string"),
                        errhint("You may have to use explicit casts.")));

    char *field = lhs_value->val.string.val;

    if (lhs_value->type != AGTV_STRING)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_trunc arg 3 must be a string"),
                        errhint("You may have to use explicit casts.")));

    char *zone_str = zone_value->val.string.val;


    gtype_value gtv;
    if (rhs_value->type == AGTV_TIMESTAMPTZ) {
        gtv.val.int_value = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_trunc_zone,
                                              CStringGetTextDatum(field),
                                              TimestampTzGetDatum(rhs_value->val.int_value),
					      CStringGetTextDatum(zone_str)));
        gtv.type = AGTV_TIMESTAMPTZ;
        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_trun(gtype, gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));
    }

    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(gtype_date_bin);
Datum
gtype_date_bin(PG_FUNCTION_ARGS) {
    gtype *stride = AG_GET_ARG_GTYPE_P(0);
    gtype *source = AG_GET_ARG_GTYPE_P(1);
    gtype *origin = AG_GET_ARG_GTYPE_P(2);

    if (is_gtype_null(stride) || is_gtype_null(source) || is_gtype_null(origin))
        PG_RETURN_NULL();

    gtype_value *stride_value = get_ith_gtype_value_from_container(&stride->root, 0);    
    gtype_value *source_value = get_ith_gtype_value_from_container(&source->root, 0);
    gtype_value *origin_value = get_ith_gtype_value_from_container(&origin->root, 0);

    if ((source_value->type == AGTV_TIMESTAMP || source_value->type == AGTV_DATE) &&
        (origin_value->type == AGTV_TIMESTAMP || origin_value->type == AGTV_DATE)) {
         Datum source_ts, origin_ts;

         if (source_value->type == AGTV_DATE)
             source_ts = gtype_to_timestamp_internal(source_value);
	 else
             source_ts = TimestampGetDatum(source_value->val.int_value);

	 if (origin_value->type == AGTV_DATE)
             origin_ts = gtype_to_timestamp_internal(origin_value);
         else
             origin_ts = TimestampGetDatum(origin_value->val.int_value);

	 
         Timestamp ts = DatumGetTimestamp(DirectFunctionCall3(timestamp_bin,
                                    IntervalPGetDatum(&stride_value->val.interval),
                                    source_ts, origin_ts));

         gtype_value gtv;
         gtv.type = AGTV_TIMESTAMP;
         gtv.val.int_value = ts;

         AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else if (source_value->type == AGTV_TIMESTAMPTZ && origin_value->type == AGTV_TIMESTAMPTZ) {
         TimestampTz ts = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_bin,
				    IntervalPGetDatum(&stride_value->val.interval),
                                    TimestampTzGetDatum(source_value->val.int_value),
                                    TimestampTzGetDatum(origin_value->val.int_value)));
         gtype_value gtv;
         gtv.type = AGTV_TIMESTAMPTZ;
         gtv.val.int_value = ts;

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else 
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input for date_bin(gtype, gtype, gtype)"),
                        errhint("You may have to use explicit casts.")));

    PG_RETURN_NULL();
}

enum overlap_type {
    ovt_timestamp,
    ovt_time,
    ovt_timetz
} overlap_type;

PG_FUNCTION_INFO_V1(gtype_overlaps);

Datum gtype_overlaps(PG_FUNCTION_ARGS)
{
    gtype *arg1 = AG_GET_ARG_GTYPE_P(0);
    gtype *arg2 = AG_GET_ARG_GTYPE_P(1);
    gtype *arg3 = AG_GET_ARG_GTYPE_P(2);
    gtype *arg4 = AG_GET_ARG_GTYPE_P(3);
    enum overlap_type type;
    bool result;
    gtype_value agtv_result;

    Datum d1;
    if (GT_IS_DATE(arg1) || GT_IS_TIMESTAMP(arg1) || GT_IS_TIMESTAMPTZ(arg1)) {
        d1 = GT_TO_TIMESTAMP_DATUM(arg1);
        type = ovt_timestamp;
    } else if (GT_IS_TIME(arg1)) {
        d1 = GT_TO_TIME_DATUM(arg1);
        type = ovt_time;
    } else if (GT_IS_TIMETZ(arg1)) {
        d1 = GT_TO_TIMETZ_DATUM(arg1);
        type = ovt_timetz;
    } else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid argument type for overlap expression")));
    }

    Datum d2;
    if (GT_IS_INTERVAL(arg2)) {
        if (type == ovt_timestamp)
            d2 = DirectFunctionCall2(timestamp_pl_interval, d1, GT_TO_INTERVAL_DATUM(arg2));
        else if (type == ovt_time)
            d2 = DirectFunctionCall2(time_pl_interval, d1, GT_TO_INTERVAL_DATUM(arg2));
        else 
            d2 = DirectFunctionCall2(timetz_pl_interval, d1, GT_TO_INTERVAL_DATUM(arg2));
    } else {
        if (type == ovt_timestamp)
            d2 = GT_TO_TIMESTAMP_DATUM(arg2);
        else if (type == ovt_time)
            d2 = GT_TO_TIME_DATUM(arg2);
        else 
            d2 = GT_TO_TIMETZ_DATUM(arg2);
    }

    Datum d3;
    if (type == ovt_timestamp)
        d3 = GT_TO_TIMESTAMP_DATUM(arg3);
    else if (type == ovt_time)
        d3 = GT_TO_TIME_DATUM(arg3);
    else 
        d3 = GT_TO_TIMETZ_DATUM(arg3);


    Datum d4;
    if (GT_IS_INTERVAL(arg4)) {
        if (type == ovt_timestamp)
            d4 = DirectFunctionCall2(timestamp_pl_interval, d3, GT_TO_INTERVAL_DATUM(arg4));
        else if (type == ovt_time)
            d4 = DirectFunctionCall2(time_pl_interval, d3, GT_TO_INTERVAL_DATUM(arg4));
        else 
            d4 = DirectFunctionCall2(timetz_pl_interval, d3, GT_TO_INTERVAL_DATUM(arg4));
    } else {
        if (type == ovt_timestamp)
            d4 = GT_TO_TIMESTAMP_DATUM(arg4);
        else if (type == ovt_time)
            d4 = GT_TO_TIME_DATUM(arg4);
        else 
            d4 = GT_TO_TIMETZ_DATUM(arg4);
    }

    if (type == ovt_timestamp)
        result = DatumGetBool(DirectFunctionCall4(overlaps_timestamp, d1, d2, d3, d4));
    else if (type == ovt_time)
        result = DatumGetBool(DirectFunctionCall4(overlaps_time, d1, d2, d3, d4));
    else 
        result = DatumGetBool(DirectFunctionCall4(overlaps_timetz, d1, d2, d3, d4));
        

    agtv_result.type = AGTV_BOOL;
    agtv_result.val.boolean = result;

    PG_RETURN_POINTER(gtype_value_to_gtype(&agtv_result));
}



PG_FUNCTION_INFO_V1(gtype_tsquery_or);
Datum gtype_tsquery_or(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (GT_IS_TSQUERY(lhs) || GT_IS_TSQUERY(rhs)) {
        TSQuery tsquery = DatumGetPointer(DirectFunctionCall2(tsquery_or,
                                                              GT_ARG_TO_TSQUERY_DATUM(0),
	  	  	  	  	  	  	      GT_ARG_TO_TSQUERY_DATUM(1)));

        gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsquery = tsquery };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
    } else {
        TSVector tsvector = DatumGetPointer(DirectFunctionCall2(tsvector_concat,
                                                              GT_ARG_TO_TSVECTOR_DATUM(0),
                                                              GT_ARG_TO_TSVECTOR_DATUM(1)));

        gtype_value gtv = { .type = AGTV_TSVECTOR, .val.tsvector = tsvector };

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));

    }
}

PG_FUNCTION_INFO_V1(gtype_tsquery_not);
Datum gtype_tsquery_not(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall1(tsquery_not, GT_ARG_TO_TSQUERY_DATUM(0)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsquery = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

/*
 * Text Search Functions
 */
PG_FUNCTION_INFO_V1(gtype_ts_delete);
Datum gtype_ts_delete(PG_FUNCTION_ARGS) {
    TSVector tsvector = DatumGetPointer(DirectFunctionCall2(tsvector_delete_str,
			                                    GT_ARG_TO_TSVECTOR_DATUM(0),
							    GT_ARG_TO_TEXT_DATUM(1)));

    gtype_value gtv = { .type = AGTV_TSVECTOR, .val.tsvector = tsvector };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_ts_strip);
Datum gtype_ts_strip(PG_FUNCTION_ARGS) {
    TSVector tsvector = DatumGetPointer(DirectFunctionCall1(tsvector_strip, GT_ARG_TO_TSVECTOR_DATUM(0)));

    gtype_value gtv = { .type = AGTV_TSVECTOR, .val.tsvector = tsvector };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_tsquery_phrase);
Datum gtype_tsquery_phrase(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall3(tsquery_phrase_distance,
                                                            GT_ARG_TO_TSQUERY_DATUM(0),
                                                            GT_ARG_TO_TSQUERY_DATUM(1),
							    Int32GetDatum(1)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsvector = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_tsquery_phrase_distance);
Datum gtype_tsquery_phrase_distance(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall3(tsquery_phrase_distance,
                                                            GT_ARG_TO_TSQUERY_DATUM(0),
                                                            GT_ARG_TO_TSQUERY_DATUM(1),
                                                            GT_ARG_TO_INT4_DATUM(2)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsvector = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_plainto_tsquery);
Datum gtype_plainto_tsquery(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall1(plainto_tsquery, GT_ARG_TO_TEXT_DATUM(0)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsvector = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_phraseto_tsquery);
Datum gtype_phraseto_tsquery(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall1(phraseto_tsquery, GT_ARG_TO_TEXT_DATUM(0)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsvector = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_websearch_to_tsquery);
Datum gtype_websearch_to_tsquery(PG_FUNCTION_ARGS) {
    TSQuery tsquery = DatumGetPointer(DirectFunctionCall1(websearch_to_tsquery, GT_ARG_TO_TEXT_DATUM(0)));

    gtype_value gtv = { .type = AGTV_TSQUERY, .val.tsvector = tsquery };

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_toupper);
// toUpper(gtype)
Datum gtype_toupper(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(upper, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};
 
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_tolower);
// toLower(gtype)
Datum gtype_tolower(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(lower, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};
 
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_rtrim);
// rtrim(gtype)
Datum gtype_rtrim(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1(rtrim1, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};
 
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_ltrim);
//ltrim(gtype)
Datum gtype_ltrim(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1(ltrim1, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};
 
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_trim);
// trim(gtype)
Datum gtype_trim(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1(btrim1, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_right);
// gtype(gtype, gtype)
Datum gtype_right(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall2(text_right, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_INT8_DATUM(1));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_left);
// left(gtype, gtype)
Datum gtype_left(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall2(text_left, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_INT8_DATUM(1));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_initcap);
// initCap(gtype)
Datum gtype_initcap(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(initcap, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(d) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_substring_w_len);
// substring(gtype, gtype, gtype)
Datum
gtype_substring_w_len(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall3(text_substr, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_INT4_DATUM(1), GT_ARG_TO_INT4_DATUM(2));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_substring);
// substring(gtype, gtype)
Datum gtype_substring(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall2(text_substr_no_len, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_INT4_DATUM(1));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_split);
// split(gtype, gtype)
Datum gtype_split(PG_FUNCTION_ARGS)
{
    Datum text_array = DirectFunctionCall2Coll(regexp_split_to_array, DEFAULT_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1));

    gtype_in_state in_state;
    memset(&in_state, 0, sizeof(gtype_in_state));

    array_to_gtype_internal(text_array, &in_state);

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(in_state.res));
}

PG_FUNCTION_INFO_V1(gtype_replace);
// replace(gtype, gtype, gtype)
Datum gtype_replace(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall3Coll(replace_text, DEFAULT_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1), GT_ARG_TO_TEXT_DATUM(2));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_like);
// gtype ~~~ gtype
Datum gtype_like(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(textlike, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gtype_not_like);
// gtype !~~ gtype
Datum gtype_not_like(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(textnlike, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gtype_ilike);
// gtype ~~* gtype
Datum gtype_ilike(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(texticlike, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}
    
PG_FUNCTION_INFO_V1(gtype_not_ilike);
// gtype !~~* gtype
Datum gtype_not_ilike(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(texticnlike, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gserialized_contains_2d);

PG_FUNCTION_INFO_V1(gtype_eq_tilde);
// gtype ~ gtype
Datum gtype_eq_tilde(PG_FUNCTION_ARGS)
{
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    if (GT_IS_GEOMETRY(lhs) || GT_IS_GEOMETRY(rhs))
        PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2(gserialized_contains_2d, GT_TO_GEOMETRY_DATUM(lhs), GT_TO_GEOMETRY_DATUM(rhs))));

    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(textregexeq, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gtype_match_case_insensitive);
// gtype ~* gtype
Datum gtype_match_case_insensitive(PG_FUNCTION_ARGS) 
{   
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(texticregexeq, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gtype_regex_not_cs);
// gtype !~ gtype
Datum gtype_regex_not_cs(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(textregexne, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}


PG_FUNCTION_INFO_V1(gtype_regex_not_ci);
// gtype !~ gtype
Datum gtype_regex_not_ci(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(texticregexne, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0), GT_ARG_TO_TEXT_DATUM(1))));
}

PG_FUNCTION_INFO_V1(gtype_sha224);
// sha224(gtype)
Datum gtype_sha224(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(sha224_bytea, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_BYTEA, .val.bytea = DatumGetPointer(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_sha256);
// sha256(gtype)
Datum gtype_sha256(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(sha256_bytea, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_BYTEA, .val.bytea = DatumGetPointer(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_sha384);
// sha384(gtype)
Datum gtype_sha384(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(sha384_bytea, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_BYTEA, .val.bytea = DatumGetPointer(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_sha512);
// sha512(gtype)
Datum gtype_sha512(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(sha512_bytea, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_BYTEA, .val.bytea = DatumGetPointer(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_md5);
// md5(gtype)
Datum gtype_md5(PG_FUNCTION_ARGS)
{
    Datum d = DirectFunctionCall1Coll(md5_text, C_COLLATION_OID, GT_ARG_TO_TEXT_DATUM(0));

    gtype_value gtv = { .type = AGTV_STRING, .val.string = { VARSIZE(d), text_to_cstring(DatumGetTextP(d)) }};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}



static Datum _make_range(PG_FUNCTION_ARGS, Datum d1, Datum d2, Oid rngtypid, enum gtype_value_type gt_type);

/*
 * Given a string representing the flags for the range type, return the flags
 * represented as a char.
 */
static char
range_parse_flags(const char *flags_str) {
    char flags = 0;

    if (flags_str[0] == '\0' || flags_str[1] == '\0' || flags_str[2] != '\0')
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("invalid range bound flags"),
                        errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
        
    switch (flags_str[0]) {
        case '[':
            flags |= RANGE_LB_INC;
            break;
        case '(':
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("invalid range bound flags"),
                            errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
    }

    switch (flags_str[1]) {
        case ']':
            flags |= RANGE_UB_INC;
            break;
        case ')':
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("invalid range bound flags"),
                            errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
    }

    return flags;
}

PG_FUNCTION_INFO_V1(gtype_intrange);
Datum gtype_intrange(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(_make_range(fcinfo,  
                                PG_ARGISNULL(0) ? (Datum) 0 : GT_ARG_TO_INT8_DATUM(0),
                                PG_ARGISNULL(1) ? (Datum) 0 : GT_ARG_TO_INT8_DATUM(1),
                                INT8RANGEOID,
				AGTV_RANGE_INT));
}

PG_FUNCTION_INFO_V1(gtype_numrange);
Datum gtype_numrange(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(_make_range(fcinfo,  
                                PG_ARGISNULL(0) ? (Datum) 0 : GT_ARG_TO_NUMERIC_DATUM(0),
                                PG_ARGISNULL(1) ? (Datum) 0 : GT_ARG_TO_NUMERIC_DATUM(1),
                                NUMRANGEOID,
				AGTV_RANGE_NUM));
}

PG_FUNCTION_INFO_V1(gtype_tsrange);
Datum gtype_tsrange(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(_make_range(fcinfo,  
                                PG_ARGISNULL(0) ? (Datum) 0 : GT_ARG_TO_TIMESTAMP_DATUM(0),
                                PG_ARGISNULL(1) ? (Datum) 0 : GT_ARG_TO_TIMESTAMP_DATUM(1),
                                TSRANGEOID,
				AGTV_RANGE_TS));
}

PG_FUNCTION_INFO_V1(gtype_tstzrange);
Datum gtype_tstzrange(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(_make_range(fcinfo,  
                                PG_ARGISNULL(0) ? (Datum) 0 : GT_ARG_TO_TIMESTAMPTZ_DATUM(0),
                                PG_ARGISNULL(1) ? (Datum) 0 : GT_ARG_TO_TIMESTAMPTZ_DATUM(1),
                                TSTZRANGEOID, 
				AGTV_RANGE_TSTZ));
}

PG_FUNCTION_INFO_V1(gtype_daterange);
Datum gtype_daterange(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(_make_range(fcinfo, 
			        PG_ARGISNULL(0) ? (Datum) 0 : GT_ARG_TO_DATE_DATUM(0),
				PG_ARGISNULL(1) ? (Datum) 0 : GT_ARG_TO_DATE_DATUM(1),
				DATERANGEOID,
				AGTV_RANGE_DATE));
}

static Datum _make_range(PG_FUNCTION_ARGS, Datum d1, Datum d2, Oid rngtypid, enum gtype_value_type gt_type) {
    RangeType *range;
    TypeCacheEntry *typcache;
    RangeBound lower;
    RangeBound upper;
    char flags;

    typcache = range_get_typcache(fcinfo, rngtypid);

    if (PG_NARGS() == 3 && PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                        errmsg("range constructor flags argument must not be null")));

    lower.val = d1;
    lower.infinite = PG_ARGISNULL(0);

    upper.val = d2;
    upper.infinite = PG_ARGISNULL(1);

    if (PG_NARGS() != 3) {
        lower.inclusive = true;
        lower.lower = true;

        upper.inclusive = false;
        upper.lower = false;
    } else {
        char flags = range_parse_flags(GT_TO_STRING(AG_GET_ARG_GTYPE_P(2)));
        lower.inclusive = (flags & RANGE_LB_INC) != 0;
        lower.lower = true;

        upper.inclusive = (flags & RANGE_UB_INC) != 0;
        upper.lower = false;
    }

    gtype_value gtv = { .type=gt_type, .val.range=make_range(typcache, &lower, &upper, false)};

    return GTYPE_P_GET_DATUM(gtype_value_to_gtype(&gtv));
}	



Datum
PostGraphDirectFunctionCall2(PGFunction func, Oid collation, bool *is_null, Datum arg1, Datum arg2)
{
        LOCAL_FCINFO(fcinfo, 3);
        fcinfo->flinfo = palloc0(sizeof(FmgrInfo));
        
        Datum           result;

        InitFunctionCallInfoData(*fcinfo, NULL, 3, collation, NULL, NULL);

        fcinfo->args[0].value = arg1;
        fcinfo->args[0].isnull = false;
        fcinfo->args[1].value = arg2;
        fcinfo->args[1].isnull = false;

        result = (*func) (fcinfo);

        /* Check for null result, since caller is clearly not expecting one */
        if (fcinfo->isnull) {
            *is_null = true;
	    return NULL;
	}

	*is_null = false;

        return result;
}


PG_FUNCTION_INFO_V1(gtype_intersection_point);
Datum
gtype_intersection_point(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    Datum d;
    bool is_null;
    if (GT_IS_LSEG(lhs) || GT_IS_LSEG(rhs)) {
        d = PostGraphDirectFunctionCall2(lseg_interpt, 100, &is_null, GT_TO_LSEG_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();

        gtype_value gtv = { .type = AGTV_POINT, .val.box=DatumGetPointP(d)};

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));

    } else if (GT_IS_LINE(lhs) || GT_IS_LINE(rhs)) {
        d = PostGraphDirectFunctionCall2(line_interpt, 100, &is_null, GT_TO_LINE_DATUM(lhs), GT_TO_LINE_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();

        gtype_value gtv = { .type = AGTV_POINT, .val.box=DatumGetPointP(d)};

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));

    } else if (GT_IS_BOX(lhs) && GT_IS_BOX(rhs)) {
        d = PostGraphDirectFunctionCall2(box_intersect, 100, &is_null, GT_TO_BOX_DATUM(lhs), GT_TO_BOX_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();

        gtype_value gtv = { .type = AGTV_BOX, .val.box=DatumGetBoxP(d)};

        AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));

    } else {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("invalid type for gtype # gtype")));
    }

    PG_RETURN_NULL();
}


PG_FUNCTION_INFO_V1(gtype_closest_point);
Datum
gtype_closest_point(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    Datum d;
    bool is_null = false;
    if (GT_IS_POINT(lhs) && GT_IS_BOX(rhs)) {
        d = PostGraphDirectFunctionCall2(close_pb, 100, &is_null, GT_TO_POINT_DATUM(lhs), GT_TO_BOX_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else if (GT_IS_POINT(lhs) && GT_IS_LSEG(rhs)) {
        d = PostGraphDirectFunctionCall2(close_ps, 100, &is_null, GT_TO_POINT_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else if (GT_IS_POINT(lhs) && GT_IS_LINE(rhs)) {
        d = PostGraphDirectFunctionCall2(close_pl, 100, &is_null, GT_TO_POINT_DATUM(lhs), GT_TO_LINE_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else if (GT_IS_LSEG(lhs) && GT_IS_BOX(rhs)) {
        d = PostGraphDirectFunctionCall2(close_sb, 100, &is_null, GT_TO_LSEG_DATUM(lhs), GT_TO_BOX_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else if (GT_IS_LSEG(lhs) && GT_IS_LSEG(rhs)) {
        d = PostGraphDirectFunctionCall2(close_lseg, 100, &is_null, GT_TO_LSEG_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else if (GT_IS_LINE(lhs) && GT_IS_LSEG(rhs)) {
        d = PostGraphDirectFunctionCall2(close_ls, 100, &is_null, GT_TO_LINE_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));

        if (is_null)
            PG_RETURN_NULL();
    } else {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("invalid type for gtype # gtype")));
    }

    gtype_value gtv = { .type = AGTV_POINT, .val.box=DatumGetPointP(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_center);
Datum
gtype_center(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum d;
    if (GT_IS_BOX(gt))
        d = DirectFunctionCall1(box_center, GT_TO_BOX_DATUM(gt));
    else if (GT_IS_LSEG(gt))
        d = DirectFunctionCall1(lseg_center, GT_TO_LSEG_DATUM(gt));
    else if (GT_IS_CIRCLE(gt))
        d = DirectFunctionCall1(circle_center, GT_TO_CIRCLE_DATUM(gt));
    else
        d = DirectFunctionCall1(poly_center, GT_TO_POLYGON_DATUM(gt));

    gtype_value gtv = { .type = AGTV_POINT, .val.box=DatumGetPointP(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_distance);
Datum
gtype_distance(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum d;
    if (GT_IS_LSEG(gt))
        d = DirectFunctionCall1(lseg_length, GT_TO_LSEG_DATUM(gt));
    else
        d = DirectFunctionCall1(path_length, GT_TO_PATH_DATUM(gt));

    gtype_value gtv = { .type = AGTV_FLOAT, .val.float_value=DatumGetFloat8(d)};
    
    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}


PG_FUNCTION_INFO_V1(gtype_vertical);
Datum
gtype_vertical(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum d;
    if (GT_IS_LSEG(gt))
        d = DirectFunctionCall1(lseg_vertical, GT_TO_LSEG_DATUM(gt));
    else
        d = DirectFunctionCall1(line_vertical, GT_TO_LINE_DATUM(gt));

    PG_RETURN_BOOL(DatumGetBool(d));
}

PG_FUNCTION_INFO_V1(gtype_horizontal);
Datum
gtype_horizontal(PG_FUNCTION_ARGS) {
    gtype *gt = AG_GET_ARG_GTYPE_P(0);

    Datum d;
    if (GT_IS_LSEG(gt))
        d = DirectFunctionCall1(lseg_horizontal, GT_TO_LSEG_DATUM(gt));
    else
        d = DirectFunctionCall1(line_horizontal, GT_TO_LINE_DATUM(gt));

    PG_RETURN_BOOL(DatumGetBool(d));
}


PG_FUNCTION_INFO_V1(gtype_perp);
Datum
gtype_perp(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    Datum d;
    if (GT_IS_LSEG(lhs) || GT_IS_LSEG(rhs))
        d = DirectFunctionCall2(lseg_perp, GT_TO_LSEG_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));
    else
        d = DirectFunctionCall2(line_perp, GT_TO_LINE_DATUM(lhs), GT_TO_LINE_DATUM(rhs));

    PG_RETURN_BOOL(DatumGetBool(d));
}

PG_FUNCTION_INFO_V1(gtype_parallel);
Datum
gtype_parallel(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    Datum d;
    if (GT_IS_LSEG(lhs) || GT_IS_LSEG(rhs))
        d = DirectFunctionCall2(lseg_parallel, GT_TO_LSEG_DATUM(lhs), GT_TO_LSEG_DATUM(rhs));
    else
        d = DirectFunctionCall2(line_parallel, GT_TO_LINE_DATUM(lhs), GT_TO_LINE_DATUM(rhs));

    PG_RETURN_BOOL(DatumGetBool(d));
}

PG_FUNCTION_INFO_V1(gtype_bound_box);
Datum
gtype_bound_box(PG_FUNCTION_ARGS) {
    gtype *lhs = AG_GET_ARG_GTYPE_P(0);
    gtype *rhs = AG_GET_ARG_GTYPE_P(1);

    bool is_null;
    Datum d = PostGraphDirectFunctionCall2(boxes_bound_box, 100, &is_null, GT_TO_BOX_DATUM(lhs), GT_TO_BOX_DATUM(rhs));

    if (is_null)
        PG_RETURN_NULL();

    gtype_value gtv = { .type = AGTV_BOX, .val.box=DatumGetBoxP(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_height);
Datum
gtype_height(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(box_height, GT_TO_BOX_DATUM(AG_GET_ARG_GTYPE_P(0)));

    gtype_value gtv = { .type = AGTV_FLOAT, .val.float_value=DatumGetFloat8(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

PG_FUNCTION_INFO_V1(gtype_width);
Datum
gtype_width(PG_FUNCTION_ARGS) {
    Datum d = DirectFunctionCall1(box_width, GT_TO_BOX_DATUM(AG_GET_ARG_GTYPE_P(0)));

    gtype_value gtv = { .type = AGTV_FLOAT, .val.float_value=DatumGetFloat8(d)};

    AG_RETURN_GTYPE_P(gtype_value_to_gtype(&gtv));
}

