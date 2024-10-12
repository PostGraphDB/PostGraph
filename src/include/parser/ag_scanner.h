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

#ifndef AG_AG_SCANNER_H
#define AG_AG_SCANNER_H

/*
 * AG_TOKEN_NULL indicates the end of a scan. The name came from YY_NULL.
 *
 * AG_TOKEN_DECIMAL can be a decimal integer literal that does not fit in "int"
 * type.
 */
typedef enum ag_token_type
{
    AG_TOKEN_NULL,
    AG_TOKEN_INTEGER,
    AG_TOKEN_DECIMAL,
    AG_TOKEN_STRING,
    AG_TOKEN_XCONST,
    AG_TOKEN_BCONST,
    AG_TOKEN_IDENTIFIER,
    AG_TOKEN_CS_IDENTIFIER,
    AG_TOKEN_PARAMETER,
    AG_TOKEN_LT_GT,
    AG_TOKEN_LT_EQ,
    AG_TOKEN_GT_EQ,
    AG_TOKEN_DOT_DOT,
    AG_TOKEN_TYPECAST,
    AG_TOKEN_OPERATOR,
    AG_TOKEN_PLUS_EQ,
    AG_TOKEN_INET,
    AG_TOKEN_CHAR
} ag_token_type;

/*
 * Fields in value field are used with the following types.
 *
 *     * c: AG_TOKEN_CHAR
 *     * i: AG_TOKEN_INTEGER
 *     * s: all other types except the types for c and i, and AG_TOKEN_NULL
 *
 * "int" type is chosen for value.i to line it up with Value in PostgreSQL.
 *
 * value.s is read-only because it points at an internal buffer and it changes
 * for every ag_scanner_next_token() call. So, users who want to keep or modify
 * the value need to copy it first.
 */
typedef struct ag_token
{
    ag_token_type type;
    union
    {
        char c;
        int i;
        const char *s;
    } value;
    int location;
} ag_token;

// an opaque data structure encapsulating the current state of the scanner
typedef void *ag_scanner_t;

ag_scanner_t ag_scanner_create(const char *s);
void ag_scanner_destroy(ag_scanner_t scanner);
ag_token ag_scanner_next_token(ag_scanner_t scanner);

int ag_scanner_errmsg(const char *msg, ag_scanner_t *scanner);
int ag_scanner_errposition(const int location, ag_scanner_t *scanner);

#endif
