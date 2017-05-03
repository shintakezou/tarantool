#include "sql.h"
#include "box/sql.h"
#include "trivia/util.h"
#include "fiber.h"
#include "small/region.h"
#include "box/memtx_tuple.h"
#include "box/port.h"

#include "sqlite3.h"
#include "box/lua/misc.h"
#include "lua/utils.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

void
lua_push_column_names(struct lua_State *L, sqlite3_stmt *stmt)
{
	int n = sqlite3_column_count(stmt);
	lua_createtable(L, n, 0);
	for (int i = 0; i < n; i++) {
		const char *name = sqlite3_column_name(stmt, i);
		lua_pushstring(L, name == NULL ? "" : name);
		lua_rawseti(L, -2, i+1);
	}
}

struct tuple *
sql_row_to_tuple(sqlite3_stmt *stmt)
{
	int column_count = sqlite3_column_count(stmt);
	assert(column_count > 0);

	/* Calculate size. */
	uint32_t mp_size = mp_sizeof_array(column_count);
	for (int i = 0; i < column_count; ++i) {
		int type = sqlite3_column_type(stmt, i);
		switch (type) {
		case SQLITE_INTEGER: {
			int n = sqlite3_column_int(stmt, i);
			if (n >= 0)
				mp_size += mp_sizeof_uint(n);
			else
				mp_size += mp_sizeof_int(n);
			break;
		}
		case SQLITE_FLOAT: {
			double d = sqlite3_column_double(stmt, i);
			mp_size += mp_sizeof_double(d);
			break;
		}
		case SQLITE_TEXT:
			mp_size += mp_sizeof_str(sqlite3_column_bytes(stmt, i));
			break;
		case SQLITE_BLOB:
			mp_size += mp_sizeof_bin(sqlite3_column_bytes(stmt, i));
			break;
		case SQLITE_NULL:
			mp_size += mp_sizeof_nil();
			break;
		default:
			unreachable();
		}
	}
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	char *mp_data = region_alloc(region, mp_size);
	char *pos = mp_data;
	if (mp_data == NULL)
		return NULL;

	/* Encode tuple data. */
	pos = mp_encode_array(pos, column_count);
	for (int i = 0; i < column_count; ++i) {
		int type = sqlite3_column_type(stmt, i);
		switch (type) {
		case SQLITE_INTEGER: {
			int n = sqlite3_column_int(stmt, i);
			if (n >= 0)
				pos = mp_encode_uint(pos, n);
			else
				pos = mp_encode_int(pos, n);
			break;
		}
		case SQLITE_FLOAT: {
			double d = sqlite3_column_double(stmt, i);
			pos = mp_encode_double(pos, d);
			break;
		}
		case SQLITE_TEXT: {
			uint32_t len = sqlite3_column_bytes(stmt, i);
			const char *text =
				(const char *) sqlite3_column_text(stmt, i);
			pos = mp_encode_str(pos, text, len);
			break;
		}
		case SQLITE_BLOB: {
			uint32_t len = sqlite3_column_bytes(stmt, i);
			const void *bin = sqlite3_column_blob(stmt, i);
			pos = mp_encode_bin(pos, bin, len);
			break;
		}
		case SQLITE_NULL:
			pos = mp_encode_nil(pos);
			break;
		default:
			unreachable();
		}
	}
	assert(mp_data + mp_size == pos);

	struct tuple *res = memtx_tuple_new(tuple_format_default, mp_data, pos);
	region_truncate(region, region_svp);
	return res;
}

int
box_sql_execute(struct port *port, const char *sql, const char *sql_end,
		bool *no_columns)
{
	int rc;
	sqlite3 *db = sql_get();
	*no_columns = true;

	if (db == NULL) {
		diag_set(ClientError, ER_SQL, "sqlite is not ready");
		return -1;
	}
	assert(sql_end - sql <= INT_MAX);
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db, sql, (int)(sql_end - sql), &stmt, &sql);
	if (rc != SQLITE_OK)
		goto sqlerror;
	if (sql != sql_end) {
		/*
		 * Check if the rest of the query contains only
		 * whitespaces. Make check in prepare_v2, because
		 * only sqlite has necessary information about
		 * current locale whitespaces.
		 */
		sqlite3_stmt *rest;
		rc = sqlite3_prepare_v2(db, sql, (int)(sql_end - sql), &rest,
					&sql);
		if (rc != SQLITE_OK)
			goto sqlerror;
		sqlite3_finalize(rest);
		if (rest != NULL) {
			diag_set(ClientError, ER_SQL, "SQL expression must contain "\
				 "single query and either nothing or ';' at the end");
			return -1;
		}
		assert(sql == sql_end);
	}

	if (stmt == NULL) {
		/* only whitespace */
		assert(sql == sql_end);
		return 0;
	}

	if (sqlite3_column_count(stmt) == 0) {
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { ; }
	} else {
		*no_columns = false;
		while ((rc = sqlite3_step(stmt) == SQLITE_ROW)) {
			struct tuple *tuple = sql_row_to_tuple(stmt);
			if (tuple == NULL || port_add_tuple(port, tuple) != 0)
				goto err;
		}
	}
	if (rc != SQLITE_OK && rc != SQLITE_DONE)
		goto sqlerror;
	sqlite3_finalize(stmt);
	return 0;
sqlerror:
	diag_set(ClientError, ER_SQL, sqlite3_errmsg(db));
err:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return -1;
}

static int
lua_sql_execute(struct lua_State *L)
{
	size_t length;
	const char *sql, *sql_end;

	sql = lua_tolstring(L, 1, &length);
	if (sql == NULL)
		return luaL_error(L, "usage: box.sql.execute(sqlstring)");

	assert(length <= INT_MAX);
	sql_end = sql + length;

	struct port port;
	port_create(&port);
	bool no_columns;
	if (box_sql_execute(&port, sql, sql_end, &no_columns) != 0) {
		port_destroy(&port);
		return luaT_error(L);
	}
	if (!no_columns)
		lbox_port_to_table(L, &port);
	port_destroy(&port);
	return no_columns ? 0 : 1;
}

void
box_lua_sqlite_init(struct lua_State *L)
{
	static const struct luaL_reg module_funcs [] = {
		{"execute", lua_sql_execute},
		{NULL, NULL}
	};

	/* used by lua_sql_execute via upvalue */
	lua_createtable(L, 0, 1);
	lua_pushstring(L, "sequence");
	lua_setfield(L, -2, "__serialize");

	luaL_openlib(L, "box.sql", module_funcs, 1);
	lua_pop(L, 1);
}

