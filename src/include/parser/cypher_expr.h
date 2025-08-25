
#ifndef POSTGRAPH_CYPHER_EXPR_H
#define POSTGRAPH_CYPHER_EXPR_H

#include "nodes/nodes.h"
#include "parser/parse_node.h"

#include "parser/cypher_parse_node.h"

Node *transform_cypher_expr(cypher_parsestate *cpstate, Node *expr,
                            ParseExprKind expr_kind);
        
List *
expand_NS_Item_Attrs(ParseState *pstate, ParseNamespaceItem *nsitem,
				  int sublevels_up, int location);

Node *cypher_columnref_hook (ParseState *pstate, ColumnRef *cref);

#endif
