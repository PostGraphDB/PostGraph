#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "mb/pg_wchar.h"

#include "miscadmin.h"

#include "storage/fd.h"

#include "access/relscan.h"

#include "utils/gtype.h"
#include "utils/gtype_typecasting.h"
#define MAX_CSV_COLS 128
typedef struct load_csv_cxt
{
    FILE *file;
    TableScanDesc scan_desc;
	char *line_buf;
	char **fields;
    TupleTableSlot *slot;
} load_csv_cxt;

static int parse_csv_line(char *line, char **fields, int max_fields);

PG_FUNCTION_INFO_V1(load_csv);
Datum load_csv(PG_FUNCTION_ARGS) {
    char *file_path = GT_ARG_TO_STRING_DATUM(0);

	load_csv_cxt *cxt = palloc(sizeof(load_csv_cxt));
    cxt->line_buf = palloc(sizeof(char) * 16384);

    //if (!(cxt->file = AllocateFile(file_path, "r")))
    if (!(cxt->file = fopen(file_path, "r")))
        ereport(ERROR, (errcode_for_file_access(), 
            errmsg("could not open file \"%s\" in: %m",  file_path)));

    cxt->fields = palloc(sizeof(char *) * MAX_CSV_COLS);
    for (int i = 0; i < MAX_CSV_COLS; i++) {
        cxt->fields[i] = palloc(sizeof(char) * 1024);
    }

    ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    rsi->returnMode = SFRM_Materialize;
    TupleDesc tupdesc = rsi->expectedDesc;

    MemoryContext old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

    TupleDesc ret_tdesc = CreateTupleDescCopy(tupdesc);
    BlessTupleDesc(ret_tdesc);

    Tuplestorestate *tuple_store = tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random, false, work_mem);

    MemoryContextSwitchTo(old_cxt);

    MemoryContext tmp_cxt = AllocSetContextCreate(CurrentMemoryContext, "LOAD CSV temporary cxt", ALLOCSET_DEFAULT_SIZES);

    /*if (fgets(line_buf, sizeof(line_buf), file) == NULL) {
        fclose(file);
        ereport(ERROR, (errmsg("could not read header line from file \"%s\"", file_path)));
    }*/

    while (fgets(cxt->line_buf, 16384, cxt->file) != NULL)
    {
        int nfields = parse_csv_line(cxt->line_buf, cxt->fields, MAX_CSV_COLS);

        old_cxt = MemoryContextSwitchTo(tmp_cxt);

        /* only care when headers are supplied
        if (nfields != num_columns) {
            SPI_finish();
            fclose(file);
            ereport(WARNING, (errmsg("skipping malformed CSV line with %d fields (expected %d)", nfields, num_columns)));
            continue;
        }*/
        gtype_in_state result;
        memset(&result, 0, sizeof(gtype_in_state));

        result.res = push_gtype_value(&result.parse_state, WGT_BEGIN_ARRAY, NULL);
        for (int i = 0; i < nfields; i++) {
            add_gtype(string_to_gtype(cxt->fields[i]), false, &result, GTYPEOID, false);
        }
        result.res = push_gtype_value(&result.parse_state, WGT_END_ARRAY, NULL);

        Datum values[1];
        bool nulls[1] = {false};
        values[0] = GTYPE_P_GET_DATUM(gtype_value_to_gtype(result.res));

        HeapTuple tuple = heap_form_tuple(ret_tdesc, values, nulls);

        tuplestore_puttuple(tuple_store, tuple);

        MemoryContextSwitchTo(old_cxt);
        MemoryContextReset(tmp_cxt);

    }

    MemoryContextDelete(tmp_cxt);

    rsi->setResult = tuple_store;
    rsi->setDesc = ret_tdesc;

    PG_RETURN_NULL();

}

/*
 * A simple, state-machine based CSV line parser.
 * It handles double-quoted fields and escaped quotes ("").
 * - line: The input string to parse.
 * - fields: An output array of strings (char*).
 * - max_fields: The allocated size of the fields array.
 * Returns the number of fields parsed.
 * Memory for the fields is allocated in the current memory context.
 */
static int
parse_csv_line(char *line, char **fields, int max_fields)
{
    char *ptr = line;
    char *start_field;
    int field_idx = 0;
    bool in_quotes = false;

    // Remove trailing newline/carriage return
    int len = strlen(line);
    if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[len-1] = '\0';
        if (len > 1 && (line[len-2] == '\r' || line[len-2] == '\n')) {
            line[len-2] = '\0';
        }
    }

    start_field = ptr;

    while (*ptr && field_idx < max_fields) {
        if (in_quotes) {
            if (*ptr == '"') {
                if (*(ptr + 1) == '"') { // Escaped quote
                    // Overwrite the second quote and move on
                    memmove(ptr, ptr + 1, strlen(ptr));
                } else {
                    in_quotes = false;
                }
            }
        } else {
            if (*ptr == '"') {
                in_quotes = true;
                if (ptr == start_field) {
                    start_field++; // Field starts after the quote
                }
            } else if (*ptr == ',') {
                *ptr = '\0'; // Terminate the field string
                fields[field_idx++] = start_field;
                start_field = ptr + 1;
            }
        }
        ptr++;
    }

    // Add the last field
    if (field_idx < max_fields) {
        // Remove trailing quote if it exists
        len = strlen(start_field);
        if (len > 0 && start_field[len - 1] == '"') {
            start_field[len - 1] = '\0';
        }
        fields[field_idx++] = start_field;
    }

    return field_idx;
}
