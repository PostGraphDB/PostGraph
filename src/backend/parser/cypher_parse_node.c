/*
 * For PostgreSQL Database Management System:
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 1996-2010, The PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * Permission to use, copy, modify, and distribute this software and its documentation for any purpose,
 * without fee, and without a written agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY
 * OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA
 * HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/primnodes.h"
#include "parser/parse_node.h"

#include "parser/cypher_parse_node.h"

// NOTE: sync the logic with make_parsestate()
cypher_parsestate *make_cypher_parsestate(cypher_parsestate *parent_cpstate)
{
    ParseState *parent_pstate = (ParseState *)parent_cpstate;
    cypher_parsestate *cpstate;
    ParseState *pstate;

    cpstate = palloc0(sizeof(*cpstate));

    pstate = (ParseState *)cpstate;

    /* Fill in fields that don't start at null/false/zero */
    pstate->parentParseState = parent_pstate;
    pstate->p_next_resno = 1;
    pstate->p_resolve_unknowns = true;

    if (parent_cpstate)
    {
        pstate->p_sourcetext = parent_pstate->p_sourcetext;
        pstate->p_queryEnv = parent_pstate->p_queryEnv;
        pstate->p_pre_columnref_hook = parent_pstate->p_pre_columnref_hook;
        pstate->p_post_columnref_hook = parent_pstate->p_post_columnref_hook;
        pstate->p_paramref_hook = parent_pstate->p_paramref_hook;
        pstate->p_coerce_param_hook = parent_pstate->p_coerce_param_hook;
        pstate->p_ref_hook_state = parent_pstate->p_ref_hook_state;

        cpstate->graph_name = parent_cpstate->graph_name;
        cpstate->graph_oid = parent_cpstate->graph_oid;
        cpstate->params = parent_cpstate->params;
    }

    return cpstate;
}

void free_cypher_parsestate(cypher_parsestate *cpstate)
{
    free_parsestate((ParseState *)cpstate);
}

/*
 * Generates a default alias name for when a query needs on and the parse
 * state does not provide one.
 */
char *get_next_default_alias(cypher_parsestate *cpstate)
{
    char *alias_name;
    int nlen = 0;

    /* get the length of the combinded string */
    nlen = snprintf(NULL, 0, "%s%d", AGE_DEFAULT_ALIAS_PREFIX,
                    cpstate->default_alias_num);

    /* allocate the space */
    alias_name = palloc0(nlen + 1);

    /* create the name */
    snprintf(alias_name, nlen + 1, "%s%d", AGE_DEFAULT_ALIAS_PREFIX,
             cpstate->default_alias_num);

    /* increment the default alias number */
    cpstate->default_alias_num++;

    return alias_name;
}
