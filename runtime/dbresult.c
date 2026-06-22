/* runtime/dbresult.c -- protocol-agnostic query result model shared by the
   PostgreSQL (pgwire.c) and MySQL (mywire.c) drivers. Own TU: a program that uses
   neither driver links none of this. Column values are stored as text; the typed
   getters parse on demand, so one decode path serves both wire formats.

   This file is Task 2 of the Phase F.1 plan: the DbResult/Row node descriptors,
   the typed accessors, and the DbException catch plumbing. The protocol drivers
   that build these nodes land in pgwire.c (F.1 Tasks 3-7) and mywire.c (F.2). */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern char __vtable_DbException[];

/* DbResult: colnames@24 rows@32 rowcount@40, size 48.
   Row:      values@24   colnames@32,          size 40. */
#define R_COLNAMES   24
#define R_ROWS       32
#define R_ROWCOUNT   40
#define R_SIZE       48
#define ROW_VALUES   24
#define ROW_COLNAMES 32
#define ROW_SIZE     40

#define HGET(n, off)    (*(void**)((char*)(n) + (off)))
#define HSET(n, off, v) (*(void**)((char*)(n) + (off)) = (void*)(v))

/* An array's length sits at +24, its element slots at +32 (the runtime array rep). */
#define ARR_LEN(a)   (*(int64_t*)((char*)(a) + 24))
#define ARR_SLOTS(a) ((void**)((char*)(a) + 32))

/* DbResult traces 2 managed slots (colnames, rows); rowcount@40 is a plain int.
   Row traces 2 (values, colnames). colnames is a shared leaf both point at, so a
   Row never points back at its DbResult -- the graph stays acyclic. */
static int64_t g_res_ti[4] = { 0, 2, R_COLNAMES, R_ROWS };
static int64_t g_res_vt[2];
static int     g_res_vt_built;
static int64_t g_row_ti[4] = { 0, 2, ROW_VALUES, ROW_COLNAMES };
static int64_t g_row_vt[2];
static int     g_row_vt_built;

static void *res_vtable(void)
{
	if (!g_res_vt_built)
	{
		g_res_vt[0] = (int64_t)&g_res_ti[0];
		g_res_vt_built = 1;
	}

	return &g_res_vt[1];
}

static void *row_vtable(void)
{
	if (!g_row_vt_built)
	{
		g_row_vt[0] = (int64_t)&g_row_ti[0];
		g_row_vt_built = 1;
	}

	return &g_row_vt[1];
}

/* ---- builders (called by the protocol drivers; ownership of the arrays passed
   in transfers to the node, except colnames which a Row retains) -------------- */

/* colnames + rows are +1 references the caller hands over. rows is NULL for a
   non-row command (DDL/INSERT/UPDATE), with rowcount holding the affected count. */
void *bzy_db_result_new(void *colnames, void *rows, int64_t rowcount)
{
	void *n = bzy_alloc(R_SIZE);
	*(void**)n = res_vtable();
	HSET(n, R_COLNAMES, colnames);
	HSET(n, R_ROWS, rows);
	*(int64_t*)((char*)n + R_ROWCOUNT) = rowcount;
	return n;
}

/* values is a +1 reference the caller hands over; colnames is the DbResult's
   shared array, retained here so every row can resolve a column name. */
void *bzy_db_row_new(void *values, void *colnames)
{
	void *n = bzy_alloc(ROW_SIZE);
	*(void**)n = row_vtable();
	HSET(n, ROW_VALUES, values);
	bzy_retain(colnames);
	HSET(n, ROW_COLNAMES, colnames);
	return n;
}

/* ---- DbException catch plumbing (the bzy_http_check pattern) ----------------- */

static __thread char g_db_errbuf[512];
static __thread int  g_db_has_error;

/* The protocol drivers call this with a static or transient message; it is copied
   so the caller's buffer need not outlive the call. The codegen-emitted
   bzy_db_check then raises it as a catchable DbException at the call site. */
void bzy_db_set_error(const char *msg)
{
	strncpy(g_db_errbuf, msg, sizeof(g_db_errbuf) - 1);
	g_db_errbuf[sizeof(g_db_errbuf) - 1] = '\0';
	g_db_has_error = 1;
}

void bzy_db_check(int64_t pc, int64_t frame)
{
	if (!g_db_has_error)
	{
		return;
	}

	void *msg = bzy_str_new(g_db_errbuf, (int64_t)strlen(g_db_errbuf));
	g_db_has_error = 0;
	void *exc = bzy_alloc(32);
	*(void**)exc = (void*)__vtable_DbException;
	*(void**)((char*)exc + 24) = msg;
	bzy_throw(exc, pc, frame);
}

/* ---- DbResult accessors ----------------------------------------------------- */

int64_t bzy_db_row_count(void *res)
{
	return *(int64_t*)((char*)res + R_ROWCOUNT);
}

int64_t bzy_db_col_count(void *res)
{
	void *c = HGET(res, R_COLNAMES);
	return c ? ARR_LEN(c) : 0;
}

/* Owned (+1) Row. An out-of-range index (or a non-row result) sets the error for
   the emitted bzy_db_check and returns NULL. */
void *bzy_db_row(void *res, int64_t i)
{
	void *rows = HGET(res, R_ROWS);
	int64_t n = rows ? ARR_LEN(rows) : 0;
	if (!rows || i < 0 || i >= n)
	{
		bzy_db_set_error("Row index out of range.");
		return NULL;
	}

	void *r = ARR_SLOTS(rows)[i];
	bzy_retain(r);
	return r;
}

/* Owned (+1) column name string. */
void *bzy_db_col_name(void *res, int64_t i)
{
	void *c = HGET(res, R_COLNAMES);
	int64_t n = c ? ARR_LEN(c) : 0;
	if (!c || i < 0 || i >= n)
	{
		bzy_db_set_error("Column index out of range.");
		return NULL;
	}

	void *s = ARR_SLOTS(c)[i];
	bzy_retain(s);
	return s;
}

/* ---- Row accessors (by index) ----------------------------------------------- */

/* Borrowed value slot; NULL = SQL NULL. Out-of-range -> NULL (treated as SQL NULL). */
static void *row_value(void *row, int64_t i)
{
	void *v = HGET(row, ROW_VALUES);
	int64_t n = v ? ARR_LEN(v) : 0;
	if (!v || i < 0 || i >= n)
	{
		return NULL;
	}

	return ARR_SLOTS(v)[i];
}

int64_t bzy_db_row_columns(void *row)
{
	void *v = HGET(row, ROW_VALUES);
	return v ? ARR_LEN(v) : 0;
}

int64_t bzy_db_is_null(void *row, int64_t i)
{
	return row_value(row, i) == NULL;
}

/* Owned (+1) string. A SQL NULL maps to "" (string == null is unsupported in
   Breezy); isNull distinguishes a real NULL from an empty string. */
void *bzy_db_get_string(void *row, int64_t i)
{
	void *s = row_value(row, i);
	if (!s)
	{
		return bzy_str_new("", 0);
	}

	bzy_retain(s);
	return s;
}

/* Serves both getInt (codegen truncates to 32-bit) and getLong. NULL -> 0. */
int64_t bzy_db_get_long(void *row, int64_t i)
{
	void *s = row_value(row, i);
	return s ? strtoll(bzy_str_data(s), NULL, 10) : 0;
}

double bzy_db_get_double(void *row, int64_t i)
{
	void *s = row_value(row, i);
	return s ? strtod(bzy_str_data(s), NULL) : 0.0;
}

/* Postgres bool text is "t"/"f"; accept "1"/"true" too. NULL -> false. */
int64_t bzy_db_get_bool(void *row, int64_t i)
{
	void *s = row_value(row, i);
	if (!s)
	{
		return 0;
	}

	const char *d = bzy_str_data(s);
	return (d[0] == 't' || d[0] == 'T' || d[0] == '1');
}

/* ---- Row accessors (by column name) ----------------------------------------- */

/* Column index for a name (case-sensitive linear scan; column sets are small).
   -1 if absent. */
static int64_t row_col_index(void *row, void *name)
{
	void *c = HGET(row, ROW_COLNAMES);
	if (!c)
	{
		return -1;
	}

	int64_t n = ARR_LEN(c);
	void **slots = ARR_SLOTS(c);
	const char *k = bzy_str_data(name);
	int64_t kl = bzy_str_len(name);
	for (int64_t i = 0; i < n; i++)
	{
		if (bzy_str_len(slots[i]) == kl && memcmp(bzy_str_data(slots[i]), k, (size_t)kl) == 0)
		{
			return i;
		}
	}

	return -1;
}

/* A name with no matching column behaves as SQL NULL (returns the type zero;
   isNull -> true). ponytail: missing-name == null, not a throw; callers that want
   strictness use columnName()/isNull. Upgrade to a throw if typo-bugs slip through. */
int64_t bzy_db_is_null_named(void *row, void *name)
{
	int64_t i = row_col_index(row, name);
	return (i < 0) ? 1 : (row_value(row, i) == NULL);
}

void *bzy_db_get_string_named(void *row, void *name)
{
	int64_t i = row_col_index(row, name);
	return (i < 0) ? bzy_str_new("", 0) : bzy_db_get_string(row, i);
}

int64_t bzy_db_get_long_named(void *row, void *name)
{
	int64_t i = row_col_index(row, name);
	return (i < 0) ? 0 : bzy_db_get_long(row, i);
}

double bzy_db_get_double_named(void *row, void *name)
{
	int64_t i = row_col_index(row, name);
	return (i < 0) ? 0.0 : bzy_db_get_double(row, i);
}

int64_t bzy_db_get_bool_named(void *row, void *name)
{
	int64_t i = row_col_index(row, name);
	return (i < 0) ? 0 : bzy_db_get_bool(row, i);
}
