#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "mb/pg_wchar.h"

#include "access/relscan.h"

#include "utils/gtype.h"
#include "utils/gtype_typecasting.h"
#define MAX_CSV_COLS 128

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

typedef struct load_csv_cxt
{
    FILE *file;
    TableScanDesc scan_desc;
	char *line_buf;
	char **fields;
    TupleTableSlot *slot;
} load_csv_cxt;

/*
 * SQL function: load_csv_manual(file_path TEXT)
 *
 * Description: Manually parses a CSV and inserts rows one-by-one.
 * Assumes the first line of the CSV is a header and skips it.
 */
PG_FUNCTION_INFO_V1(load_csv);
Datum load_csv(PG_FUNCTION_ARGS) {

    int nrows = 0;
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL()) {
        char *file_path = GT_ARG_TO_STRING_DATUM(0);
		
        MemoryContext oldcontext;


		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		load_csv_cxt *cxt= palloc(sizeof(load_csv_cxt));

        cxt->line_buf = palloc(sizeof(char) * 16384); // 16KB buffer for a single line

        if ((cxt->file = fopen(file_path, "r")) == NULL)
            ereport(ERROR, (errcode_for_file_access(), errmsg("could not open file \"%s\" for reading: %m", file_path)));

        cxt->fields = palloc(sizeof(char *) * MAX_CSV_COLS);
        for (int i = 0; i < MAX_CSV_COLS; i++) {
            cxt->fields[i] = palloc(sizeof(char) * 1024); // Allocate 1KB per field
        }

		TupleDesc tupdesc = CreateTemplateTupleDesc(1);

		TupleDescInitEntry(tupdesc, 1, "value", GTYPEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);


        funcctx->user_fctx = cxt;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	load_csv_cxt *cxt = (load_csv_cxt *) funcctx->user_fctx;
    // 2. Read header line and discard it
    /*if (fgets(line_buf, sizeof(line_buf), file) == NULL) {
        fclose(file);
        ereport(ERROR, (errmsg("could not read header line from file \"%s\"", file_path)));
    }*/

    // 4. Loop through the file, parsing and inserting each row
    while (fgets(cxt->line_buf, sizeof(cxt->line_buf), cxt->file) != NULL)
    {
        int nfields = parse_csv_line(cxt->line_buf, cxt->fields, MAX_CSV_COLS);

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

        for (int i = 0; i < nfields; i++)
            add_gtype(string_to_gtype(cxt->fields[i]), false, &result, GTYPEOID, false);

        result.res = push_gtype_value(&result.parse_state, WGT_END_ARRAY, NULL);

 		Datum values[1];
		bool nulls[1];
		values[0] = GTYPE_P_GET_DATUM(gtype_value_to_gtype(result.res));
        nulls[0] = false;
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
    }
    
	SRF_RETURN_DONE(funcctx);
}
/*
 * SQL function: load_csv_manual(table_name TEXT, file_path TEXT)
 *
 * Description: Manually parses a CSV and inserts rows one-by-one.
 * Assumes the first line of the CSV is a header and skips it.
 */
/*
Datum
load_csv_manual(PG_FUNCTION_ARGS)
{
    char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *file_path = text_to_cstring(PG_GETARG_TEXT_PP(1));

    FILE *file;
    char line_buf[16384]; // 16KB buffer for a single line
    char *fields[MAX_CSV_COLS];
    int nfields;
    int nrows = 0;

    SPIPlanPtr prep_plan;
    Oid argtypes[MAX_CSV_COLS];
    Datum values[MAX_CSV_COLS];
    char nulls[MAX_CSV_COLS];

    // 1. Open the CSV file from the server's filesystem
    if ((file = fopen(file_path, "r")) == NULL)
        ereport(ERROR, (errcode_for_file_access(), errmsg("could not open file \"%s\" for reading: %m", file_path)));

    // 2. Read header line and discard it
    if (fgets(line_buf, sizeof(line_buf), file) == NULL) {
        fclose(file);
        ereport(ERROR, (errmsg("could not read header line from file \"%s\"", file_path)));
    }

    // 3. Connect to SPI and prepare the INSERT statement
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    // NOTE: This is a simplified example assuming a specific table structure.
    // A more robust function would query pg_attribute and pg_type to get
    // the actual column types and build the INSERT statement dynamically.
    int num_columns = 3; // Hardcoded for 'users' table (id INT, name TEXT, city TEXT)
    const char *insert_sql = "INSERT INTO users (id, name, city) VALUES ($1, $2, $3)";

    argtypes[0] = INT4OID;   // OID for INTEGER
    argtypes[1] = TEXTOID;   // OID for TEXT
    argtypes[2] = TEXTOID;   // OID for TEXT

    prep_plan = SPI_prepare(insert_sql, num_columns, argtypes);
    if (prep_plan == NULL)
        elog(ERROR, "SPI_prepare failed for command: %s", insert_sql);

    // 4. Loop through the file, parsing and inserting each row
    while (fgets(line_buf, sizeof(line_buf), file) != NULL)
    {
        nfields = parse_csv_line(line_buf, fields, MAX_CSV_COLS);

        if (nfields != num_columns) {
            SPI_finish();
            fclose(file);
            ereport(WARNING, (errmsg("skipping malformed CSV line with %d fields (expected %d)", nfields, num_columns)));
            continue;
        }

        // Prepare values for insertion by calling the type's input function
        values[0] = DirectFunctionCall1(int4in, CStringGetDatum(fields[0]));
        values[1] = CStringGetTextDatum(fields[1]);
        values[2] = CStringGetTextDatum(fields[2]);

        // Assume no nulls for this example (' ' means not null)
        memset(nulls, ' ', num_columns);

        if (SPI_execute_plan(prep_plan, values, nulls, false, 0) != SPI_OK_INSERT) {
            elog(ERROR, "SPI_execute_plan failed");
        }

        nrows++;
    }

    // 5. Clean up
    SPI_finish();
    fclose(file);
    pfree(table_name);
    pfree(file_path);

    PG_RETURN_INT32(nrows);
}*/