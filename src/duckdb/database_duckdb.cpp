//
//  database_duckdb.cpp
//  cloudsync
//
//  DuckDB implementation of the database abstraction layer (database.h).
//  Uses DuckDB C++ API with extern "C" linkage for functions called from C code.
//

#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckvalue.h"
#include <atomic>

extern "C" {
#include "../database.h"
#include "../sql.h"
}

// Include CloudSync headers (has extern "C" guards)
#include "../cloudsync.h"

extern "C" {
#include "../dbutils.h"
#include "../utils.h"
}

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <map>

using namespace duckdb;

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

// duck_stmt_t is defined in duckvalue.h

// MARK: - Helpers

static Connection *get_conn(cloudsync_context *data) {
    return (Connection *)cloudsync_db(data);
}

int duckvalue_map_type(LogicalTypeId type_id) {
    switch (type_id) {
        case LogicalTypeId::TINYINT:
        case LogicalTypeId::SMALLINT:
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::BIGINT:
        case LogicalTypeId::UTINYINT:
        case LogicalTypeId::USMALLINT:
        case LogicalTypeId::UINTEGER:
        case LogicalTypeId::UBIGINT:
        case LogicalTypeId::BOOLEAN:
        case LogicalTypeId::HUGEINT:
            return DBTYPE_INTEGER;

        case LogicalTypeId::FLOAT:
        case LogicalTypeId::DOUBLE:
        case LogicalTypeId::DECIMAL:
            return DBTYPE_FLOAT;

        case LogicalTypeId::BLOB:
            return DBTYPE_BLOB;

        default:
            return DBTYPE_TEXT;
    }
}

static char *sql_escape_character(const char *name, char *buffer, size_t bsize, char c) {
    if (!name || !buffer || bsize < 1) {
        if (buffer && bsize > 0) buffer[0] = '\0';
        return NULL;
    }

    size_t i = 0, j = 0;
    while (name[i]) {
        if (name[i] == c) {
            if (j >= bsize - 2) break;
            buffer[j++] = c;
            buffer[j++] = c;
        } else {
            if (j >= bsize - 1) break;
            buffer[j++] = name[i];
        }
        i++;
    }

    buffer[j] = '\0';
    return buffer;
}

static char *sql_escape_identifier(const char *name, char *buffer, size_t bsize) {
    return sql_escape_character(name, buffer, bsize, '"');
}

static char *sql_escape_literal(const char *name, char *buffer, size_t bsize) {
    return sql_escape_character(name, buffer, bsize, '\'');
}

// MARK: - SQL builders

extern "C" char *sql_build_drop_table(const char *table_name, char *buffer, int bsize, bool is_meta) {
    char escaped[512];
    sql_escape_identifier(table_name, escaped, sizeof(escaped));

    if (is_meta) {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s_cloudsync\";", escaped);
    } else {
        snprintf(buffer, bsize, "DROP TABLE IF EXISTS \"%s\";", escaped);
    }

    return buffer;
}

extern "C" char *sql_escape_identifier_c(const char *name, char *buffer, size_t bsize) {
    return sql_escape_identifier(name, buffer, bsize);
}

extern "C" char *sql_build_select_nonpk_by_pk(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_NONPK_COLS_BY_PK, esc, esc, esc);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

extern "C" char *sql_build_delete_by_pk(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_DELETE_ROW_BY_PK, esc, esc);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

extern "C" char *sql_build_insert_pk_ignore(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_INSERT_PK_IGNORE, esc, esc);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

extern "C" char *sql_build_upsert_pk_and_col(cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc_table[512];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_UPSERT_PK_AND_COL, esc_table, esc_table, colname, colname);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

extern "C" char *sql_build_select_cols_by_pk(cloudsync_context *data, const char *table_name, const char *colname, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc_table[512];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    char *sql = cloudsync_memory_mprintf(SQL_BUILD_SELECT_COLS_BY_PK_FMT, esc_table, colname, esc_table);
    if (!sql) return NULL;

    char *query = NULL;
    int rc = database_select_text(data, sql, &query);
    cloudsync_memory_free(sql);

    return (rc == DBRES_OK) ? query : NULL;
}

extern "C" char *sql_build_rekey_pk_and_reset_version_except_col(cloudsync_context *data, const char *table_name, const char *except_col) {
    char *meta_ref = database_build_meta_ref(cloudsync_schema(data), table_name);
    if (!meta_ref) return NULL;

    char *result = cloudsync_memory_mprintf(SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL,
        meta_ref, meta_ref, except_col);
    cloudsync_memory_free(meta_ref);
    return result;
}

extern "C" char *database_table_schema(const char *table_name) {
    UNUSED_PARAMETER(table_name);
    return cloudsync_string_dup("main");
}

extern "C" char *database_build_meta_ref(const char *schema, const char *table_name) {
    char escaped_table[512];
    sql_escape_identifier(table_name, escaped_table, sizeof(escaped_table));
    if (schema && schema[0]) {
        char escaped_schema[512];
        sql_escape_identifier(schema, escaped_schema, sizeof(escaped_schema));
        return cloudsync_memory_mprintf("\"%s\".\"%s_cloudsync\"", escaped_schema, escaped_table);
    }
    return cloudsync_memory_mprintf("\"%s_cloudsync\"", escaped_table);
}

extern "C" char *database_build_base_ref(const char *schema, const char *table_name) {
    char escaped_table[512];
    sql_escape_identifier(table_name, escaped_table, sizeof(escaped_table));
    if (schema && schema[0]) {
        char escaped_schema[512];
        sql_escape_identifier(schema, escaped_schema, sizeof(escaped_schema));
        return cloudsync_memory_mprintf("\"%s\".\"%s\"", escaped_schema, escaped_table);
    }
    return cloudsync_memory_mprintf("\"%s\"", escaped_table);
}

extern "C" char *sql_build_delete_cols_not_in_schema_query(const char *schema, const char *table_name, const char *meta_ref, const char *pkcol) {
    UNUSED_PARAMETER(schema);

    char esc_table[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    return cloudsync_memory_mprintf(
        "DELETE FROM %s WHERE col_name NOT IN ("
        "SELECT name FROM pragma_table_info('%s') UNION SELECT '%s'"
        ");",
        meta_ref, esc_table, pkcol
    );
}

extern "C" char *sql_build_pk_collist_query(const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);

    char esc_table[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    return cloudsync_memory_mprintf(
        "SELECT string_agg('\"' || name || '\"', ',' ORDER BY pk) "
        "FROM pragma_table_info('%s') WHERE pk>0;",
        esc_table
    );
}

extern "C" char *sql_build_pk_decode_selectlist_query(const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);

    char esc_table[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    return cloudsync_memory_mprintf(
        "SELECT string_agg("
        "'cloudsync_pk_decode(pk, ' || CAST(pk AS VARCHAR) || ') AS \"' || name || '\"', ',' ORDER BY pk"
        ") "
        "FROM pragma_table_info('%s') WHERE pk>0;",
        esc_table
    );
}

extern "C" char *sql_build_pk_qualified_collist_query(const char *schema, const char *table_name) {
    UNUSED_PARAMETER(schema);

    char esc_table[1024];
    sql_escape_literal(table_name, esc_table, sizeof(esc_table));

    return cloudsync_memory_mprintf(
        "SELECT string_agg('\"' || name || '\"', ',' ORDER BY pk) "
        "FROM pragma_table_info('%s') WHERE pk>0;",
        esc_table
    );
}

extern "C" char *sql_build_insert_missing_pks_query(const char *schema, const char *table_name,
                                                      const char *pkvalues_identifiers,
                                                      const char *base_ref, const char *meta_ref,
                                                      const char *filter) {
    UNUSED_PARAMETER(schema);

    // DuckDB: Insert sentinel rows directly into the metadata table instead of
    // calling cloudsync_insert() as a SQL function.  cloudsync_insert() uses
    // prepared->Execute on the same Connection, which deadlocks because
    // DuckDB connections are not re-entrant.
    // The per-column metadata rows are filled by cloudsync_refill_metatable's
    // second loop (SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL).
    if (filter) {
        return cloudsync_memory_mprintf(
            "INSERT INTO %s (pk, col_name, col_version, db_version, site_id, seq) "
            "SELECT cloudsync_pk_encode(%s), '" CLOUDSYNC_TOMBSTONE_VALUE "', 1, 0, 0, 0 "
            "FROM %s b "
            "WHERE (%s) AND NOT EXISTS ("
            "    SELECT 1 FROM %s m WHERE m.pk = cloudsync_pk_encode(%s)"
            ");",
            meta_ref, pkvalues_identifiers, base_ref, filter, meta_ref, pkvalues_identifiers
        );
    }
    return cloudsync_memory_mprintf(
        "INSERT INTO %s (pk, col_name, col_version, db_version, site_id, seq) "
        "SELECT cloudsync_pk_encode(%s), '" CLOUDSYNC_TOMBSTONE_VALUE "', 1, 0, 0, 0 "
        "FROM %s b "
        "WHERE NOT EXISTS ("
        "    SELECT 1 FROM %s m WHERE m.pk = cloudsync_pk_encode(%s)"
        ");",
        meta_ref, pkvalues_identifiers, base_ref, meta_ref, pkvalues_identifiers
    );
}

// MARK: - Private helpers for single-value queries

static int database_select1_value(cloudsync_context *data, const char *sql, char **ptr_value, int64_t *int_value, DBTYPE expected_type) {
    if (ptr_value) *ptr_value = NULL;
    if (int_value) *int_value = 0;

    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        auto result = conn->Query(sql);
        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }

        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) {
            return DBRES_OK;
        }

        auto val = chunk->GetValue(0, 0);
        if (val.IsNull()) {
            return DBRES_OK;
        }

        if (expected_type == DBTYPE_INTEGER) {
            if (int_value) *int_value = val.GetValue<int64_t>();
        } else if (expected_type == DBTYPE_TEXT) {
            string str = val.ToString();
            char *ptr = (char *)cloudsync_memory_alloc(str.size() + 1);
            if (!ptr) return cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
            memcpy(ptr, str.c_str(), str.size());
            ptr[str.size()] = '\0';
            if (ptr_value) *ptr_value = ptr;
            if (int_value) *int_value = (int64_t)str.size();
        } else if (expected_type == DBTYPE_BLOB) {
            string blob_str = val.GetValueUnsafe<string>();
            if (!blob_str.empty()) {
                char *ptr = (char *)cloudsync_memory_alloc(blob_str.size());
                if (!ptr) return cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);
                memcpy(ptr, blob_str.data(), blob_str.size());
                if (ptr_value) *ptr_value = ptr;
                if (int_value) *int_value = (int64_t)blob_str.size();
            }
        }

        return DBRES_OK;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

static int database_select3_values(cloudsync_context *data, const char *sql, char **value, int64_t *len, int64_t *value2, int64_t *value3) {
    *value = NULL;
    *value2 = 0;
    *value3 = 0;
    *len = 0;

    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        auto result = conn->Query(sql);
        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }

        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) return DBRES_OK;

        // First column - text/blob
        auto val0 = chunk->GetValue(0, 0);
        if (!val0.IsNull()) {
            auto &col_type = result->types[0];
            if (col_type.id() == LogicalTypeId::BLOB) {
                string blob_str = val0.GetValueUnsafe<string>();
                if (!blob_str.empty()) {
                    char *ptr = (char *)cloudsync_memory_alloc(blob_str.size());
                    if (ptr) {
                        memcpy(ptr, blob_str.data(), blob_str.size());
                        *value = ptr;
                        *len = blob_str.size();
                    }
                }
            } else {
                string str = val0.ToString();
                if (!str.empty()) {
                    char *ptr = (char *)cloudsync_memory_alloc(str.size() + 1);
                    if (ptr) {
                        memcpy(ptr, str.c_str(), str.size());
                        ptr[str.size()] = '\0';
                        *value = ptr;
                        *len = str.size();
                    }
                }
            }
        }

        // Second column - int
        if (chunk->ColumnCount() > 1) {
            auto val1 = chunk->GetValue(1, 0);
            if (!val1.IsNull()) *value2 = val1.GetValue<int64_t>();
        }

        // Third column - int
        if (chunk->ColumnCount() > 2) {
            auto val2 = chunk->GetValue(2, 0);
            if (!val2.IsNull()) *value3 = val2.GetValue<int64_t>();
        }

        return DBRES_OK;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

// MARK: - General database operations

extern "C" int database_exec(cloudsync_context *data, const char *sql) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);

    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        auto result = conn->Query(sql);
        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }
        return DBRES_OK;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

extern "C" int database_exec_callback(cloudsync_context *data, const char *sql,
                                       int (*callback)(void *xdata, int argc, char **values, char **names),
                                       void *xdata) {
    if (!sql) return cloudsync_set_error(data, "SQL statement is NULL", DBRES_ERROR);
    cloudsync_reset_error(data);

    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        auto result = conn->Query(sql);
        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }

        if (!callback) return DBRES_OK;

        // Get column names
        auto &col_names = result->names;
        int ncols = (int)col_names.size();

        char **names = (char **)cloudsync_memory_alloc(ncols * sizeof(char *));
        if (!names) return DBRES_NOMEM;
        for (int i = 0; i < ncols; i++) {
            names[i] = cloudsync_string_dup(col_names[i].c_str());
        }

        // Fetch all chunks and process rows
        int rc = DBRES_OK;
        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;

            for (idx_t row = 0; row < chunk->size(); row++) {
                char **values = (char **)cloudsync_memory_alloc(ncols * sizeof(char *));
                if (!values) { rc = DBRES_NOMEM; break; }

                for (int col = 0; col < ncols; col++) {
                    auto val = chunk->GetValue(col, row);
                    if (val.IsNull()) {
                        values[col] = NULL;
                    } else {
                        string str = val.ToString();
                        values[col] = (char *)cloudsync_memory_alloc(str.size() + 1);
                        if (values[col]) {
                            memcpy(values[col], str.c_str(), str.size());
                            values[col][str.size()] = '\0';
                        }
                    }
                }

                int cb_rc = callback(xdata, ncols, values, names);

                for (int col = 0; col < ncols; col++) {
                    if (values[col]) cloudsync_memory_free(values[col]);
                }
                cloudsync_memory_free(values);

                if (cb_rc != 0) {
                    rc = cloudsync_set_error(data, "database_exec_callback aborted", DBRES_ABORT);
                    break;
                }
            }

            if (rc != DBRES_OK) break;
        }

        for (int i = 0; i < ncols; i++) {
            if (names[i]) cloudsync_memory_free(names[i]);
        }
        cloudsync_memory_free(names);

        return rc;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

extern "C" int database_write(cloudsync_context *data, const char *sql,
                               const char **bind_values, DBTYPE bind_types[], int bind_lens[], int bind_count) {
    if (!sql) return cloudsync_set_error(data, "Invalid parameters to database_write", DBRES_ERROR);
    cloudsync_reset_error(data);

    dbvm_t *stmt;
    int rc = databasevm_prepare(data, sql, &stmt, 0);
    if (rc != DBRES_OK) return rc;

    for (int i = 0; i < bind_count; i++) {
        int param_idx = i + 1;

        switch (bind_types[i]) {
            case DBTYPE_NULL:
                rc = databasevm_bind_null(stmt, param_idx);
                break;
            case DBTYPE_INTEGER: {
                int64_t val = strtoll(bind_values[i], NULL, 0);
                rc = databasevm_bind_int(stmt, param_idx, val);
                break;
            }
            case DBTYPE_FLOAT: {
                double val = strtod(bind_values[i], NULL);
                rc = databasevm_bind_double(stmt, param_idx, val);
                break;
            }
            case DBTYPE_TEXT:
                rc = databasevm_bind_text(stmt, param_idx, bind_values[i], bind_lens[i]);
                break;
            case DBTYPE_BLOB:
                rc = databasevm_bind_blob(stmt, param_idx, bind_values[i], bind_lens[i]);
                break;
            default:
                rc = DBRES_ERROR;
                break;
        }

        if (rc != DBRES_OK) {
            databasevm_finalize(stmt);
            return rc;
        }
    }

    rc = databasevm_step(stmt);
    databasevm_finalize(stmt);
    return (rc == DBRES_DONE || rc == DBRES_ROW) ? DBRES_OK : rc;
}

extern "C" int database_select_int(cloudsync_context *data, const char *sql, int64_t *value) {
    return database_select1_value(data, sql, NULL, value, DBTYPE_INTEGER);
}

extern "C" int database_select_text(cloudsync_context *data, const char *sql, char **value) {
    int64_t len = 0;
    return database_select1_value(data, sql, value, &len, DBTYPE_TEXT);
}

extern "C" int database_select_blob(cloudsync_context *data, const char *sql, char **value, int64_t *len) {
    return database_select1_value(data, sql, value, len, DBTYPE_BLOB);
}

extern "C" int database_select_blob_2int(cloudsync_context *data, const char *sql, char **value, int64_t *value_len, int64_t *value2, int64_t *value3) {
    return database_select3_values(data, sql, value, value_len, value2, value3);
}

// MARK: - Table/trigger existence

extern "C" bool database_table_exists(cloudsync_context *data, const char *table_name, const char *schema) {
    if (!table_name) return false;
    cloudsync_reset_error(data);

    Connection *conn = get_conn(data);
    if (!conn) return false;

    try {
        string query = "SELECT 1 FROM information_schema.tables WHERE table_name = '" +
                        string(table_name) + "'";
        if (schema && schema[0]) {
            query += " AND table_schema = '" + string(schema) + "'";
        }
        query += " LIMIT 1;";

        auto result = conn->Query(query);
        if (result->HasError()) return false;

        auto chunk = result->Fetch();
        return (chunk && chunk->size() > 0);
    } catch (...) {
        return false;
    }
}

extern "C" bool database_internal_table_exists(cloudsync_context *data, const char *name) {
    return database_table_exists(data, name, NULL);
}

extern "C" bool database_trigger_exists(cloudsync_context *data, const char *table_name) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(table_name);
    // DuckDB does not support triggers
    return false;
}

// MARK: - Metatable and triggers

extern "C" int database_create_metatable(cloudsync_context *data, const char *table_name) {
    char *meta_ref = database_build_meta_ref(cloudsync_schema(data), table_name);
    if (!meta_ref) return cloudsync_set_error(data, "Unable to build meta table ref", DBRES_ERROR);

    char *sql = cloudsync_memory_mprintf(
        "CREATE TABLE IF NOT EXISTS %s ("
        "pk BLOB NOT NULL, "
        "col_name VARCHAR NOT NULL, "
        "col_version BIGINT, "
        "db_version BIGINT, "
        "site_id BIGINT DEFAULT 0, "
        "seq BIGINT, "
        "PRIMARY KEY (pk, col_name)"
        ");", meta_ref);
    cloudsync_memory_free(meta_ref);

    if (!sql) return cloudsync_set_error(data, "Memory allocation failed", DBRES_NOMEM);

    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

extern "C" int database_create_triggers(cloudsync_context *data, const char *table_name, table_algo algo, const char *filter) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(table_name);
    UNUSED_PARAMETER(algo);
    UNUSED_PARAMETER(filter);
    // DuckDB does not support triggers.
    // Change tracking must be done via explicit function calls:
    //   cloudsync_insert(), cloudsync_update(), cloudsync_delete()
    return DBRES_OK;
}

extern "C" int database_delete_triggers(cloudsync_context *data, const char *table_name) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(table_name);
    // DuckDB does not support triggers
    return DBRES_OK;
}

// MARK: - Primary key info

extern "C" int database_pk_names(cloudsync_context *data, const char *table_name, char ***names, int *count) {
    *names = NULL;
    *count = 0;

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(
        "SELECT name FROM pragma_table_info('%s') WHERE pk>0 ORDER BY pk;", esc);
    if (!sql) return DBRES_NOMEM;

    Connection *conn = get_conn(data);
    if (!conn) {
        cloudsync_memory_free(sql);
        return cloudsync_set_error(data, "No database connection", DBRES_ERROR);
    }

    try {
        auto result = conn->Query(sql);
        cloudsync_memory_free(sql);
        sql = NULL;

        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }

        // Collect names
        int cap = 8;
        int n = 0;
        char **arr = (char **)cloudsync_memory_alloc(cap * sizeof(char *));
        if (!arr) return DBRES_NOMEM;

        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;

            for (idx_t row = 0; row < chunk->size(); row++) {
                auto val = chunk->GetValue(0, row);
                if (val.IsNull()) continue;

                if (n >= cap) {
                    cap *= 2;
                    arr = (char **)cloudsync_memory_realloc(arr, cap * sizeof(char *));
                    if (!arr) return DBRES_NOMEM;
                }

                string name_str = val.ToString();
                arr[n] = cloudsync_string_dup(name_str.c_str());
                n++;
            }
        }

        *names = arr;
        *count = n;
        return DBRES_OK;
    } catch (std::exception &e) {
        if (sql) cloudsync_memory_free(sql);
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

extern "C" int database_cleanup(cloudsync_context *data) {
    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        auto result = conn->Query(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema='main' AND table_name NOT LIKE 'cloudsync_%' "
            "AND table_name NOT LIKE '%\\_cloudsync' ESCAPE '\\';"
        );
        if (result->HasError()) {
            return cloudsync_set_error(data, result->GetError().c_str(), DBRES_ERROR);
        }

        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;

            for (idx_t row = 0; row < chunk->size(); row++) {
                auto val = chunk->GetValue(0, row);
                if (val.IsNull()) continue;
                string tbl = val.ToString();
                int rc = cloudsync_cleanup(data, tbl.c_str());
                if (rc != DBRES_OK) return rc;
            }
        }
        return DBRES_OK;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

// MARK: - Column counting

extern "C" int database_count_pk(cloudsync_context *data, const char *table_name, bool not_null, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql;
    if (not_null) {
        sql = cloudsync_memory_mprintf(
            "SELECT count(*) FROM pragma_table_info('%s') WHERE pk>0 AND \"notnull\"=true;", esc);
    } else {
        sql = cloudsync_memory_mprintf(
            "SELECT count(*) FROM pragma_table_info('%s') WHERE pk>0;", esc);
    }
    if (!sql) return 0;

    int64_t count = 0;
    database_select_int(data, sql, &count);
    cloudsync_memory_free(sql);
    return (int)count;
}

extern "C" int database_count_nonpk(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(
        "SELECT count(*) FROM pragma_table_info('%s') WHERE pk=0;", esc);
    if (!sql) return 0;

    int64_t count = 0;
    database_select_int(data, sql, &count);
    cloudsync_memory_free(sql);
    return (int)count;
}

extern "C" int database_count_int_pk(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(
        "SELECT count(*) FROM pragma_table_info('%s') WHERE pk>0 AND (type LIKE '%%INT%%' OR type LIKE '%%int%%');", esc);
    if (!sql) return 0;

    int64_t count = 0;
    database_select_int(data, sql, &count);
    cloudsync_memory_free(sql);
    return (int)count;
}

extern "C" int database_count_notnull_without_default(cloudsync_context *data, const char *table_name, const char *schema) {
    UNUSED_PARAMETER(schema);

    char esc[512];
    sql_escape_literal(table_name, esc, sizeof(esc));

    char *sql = cloudsync_memory_mprintf(
        "SELECT count(*) FROM pragma_table_info('%s') WHERE pk=0 AND \"notnull\"=true AND dflt_value IS NULL;", esc);
    if (!sql) return 0;

    int64_t count = 0;
    database_select_int(data, sql, &count);
    cloudsync_memory_free(sql);
    return (int)count;
}

// MARK: - Schema version

extern "C" int64_t database_schema_version(cloudsync_context *data) {
    int64_t version = 0;
    database_select_int(data, SQL_SCHEMA_VERSION, &version);
    return version;
}

extern "C" uint64_t database_schema_hash(cloudsync_context *data) {
    int64_t value = 0;
    int rc = database_select_int(data, "SELECT hash FROM cloudsync_schema_versions ORDER BY seq DESC LIMIT 1;", &value);
    return (rc == DBRES_OK) ? (uint64_t)value : 0;
}

extern "C" bool database_check_schema_hash(cloudsync_context *data, uint64_t hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM cloudsync_schema_versions WHERE hash = %" PRId64, (int64_t)hash);

    int64_t value = 0;
    database_select_int(data, sql, &value);
    return (value == 1);
}

extern "C" int database_update_schema_hash(cloudsync_context *data, uint64_t *hash) {
    // Build normalized schema string using only: column name (lowercase), type (SQLite affinity), pk flag
    // Format: tablename:colname:affinity:pk,... (ordered by table name, then column ordinal position)
    // This makes the hash portable across databases.
    //
    // DuckDB type to SQLite affinity mapping:
    // - INTEGER, SMALLINT, BIGINT, TINYINT, BOOLEAN → 'integer'
    // - BLOB → 'blob'
    // - FLOAT, DOUBLE, REAL → 'real'
    // - DECIMAL, NUMERIC → 'numeric'
    // - Everything else → 'text'

    char *schema = NULL;
    int rc = database_select_text(data,
        "SELECT string_agg("
        "    LOWER(c.table_name) || ':' || LOWER(c.column_name) || ':' || "
        "    CASE "
        "        WHEN c.data_type IN ('INTEGER', 'SMALLINT', 'BIGINT', 'TINYINT', 'BOOLEAN', 'HUGEINT') THEN 'integer' "
        "        WHEN c.data_type = 'BLOB' THEN 'blob' "
        "        WHEN c.data_type IN ('FLOAT', 'DOUBLE', 'REAL') THEN 'real' "
        "        WHEN c.data_type IN ('DECIMAL', 'NUMERIC') THEN 'numeric' "
        "        ELSE 'text' "
        "    END || ':' || "
        "    CASE WHEN tc_col.column_name IS NOT NULL THEN '1' ELSE '0' END, "
        "    ',' ORDER BY c.table_name, c.ordinal_position"
        ") "
        "FROM information_schema.columns c "
        "JOIN cloudsync_table_settings cts ON LOWER(c.table_name) = LOWER(cts.tbl_name) "
        "LEFT JOIN information_schema.table_constraints tc "
        "    ON tc.table_name = c.table_name "
        "    AND tc.table_schema = c.table_schema "
        "    AND tc.constraint_type = 'PRIMARY KEY' "
        "LEFT JOIN information_schema.key_column_usage tc_col "
        "    ON tc_col.table_name = c.table_name "
        "    AND tc_col.column_name = c.column_name "
        "    AND tc_col.table_schema = c.table_schema "
        "    AND tc_col.constraint_name = tc.constraint_name "
        "WHERE c.table_schema = 'main'",
        &schema);

    if (rc != DBRES_OK || !schema) return cloudsync_set_error(data, "database_update_schema_hash error 1", DBRES_ERROR);

    size_t schema_len = strlen(schema);
    uint64_t h = fnv1a_hash(schema, schema_len);
    cloudsync_memory_free(schema);
    if (hash && *hash == h) return cloudsync_set_error(data, "database_update_schema_hash constraint", DBRES_CONSTRAINT);

    // DuckDB does not allow subqueries in ON CONFLICT DO UPDATE SET,
    // so compute next seq first, then use it as a literal.
    int64_t next_seq = 1;
    database_select_int(data, "SELECT COALESCE(MAX(seq), 0) + 1 FROM cloudsync_schema_versions;", &next_seq);

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cloudsync_schema_versions (hash, seq) "
             "VALUES (%" PRId64 ", %" PRId64 ") "
             "ON CONFLICT(hash) DO UPDATE SET "
             "seq = %" PRId64 ";",
             (int64_t)h, next_seq, next_seq);
    rc = database_exec(data, sql);
    if (rc == DBRES_OK) {
        if (hash) *hash = h;
        return rc;
    }

    return cloudsync_set_error(data, "database_update_schema_hash error 2", DBRES_ERROR);
}

// MARK: - Transaction/savepoint

extern "C" int database_begin_savepoint(cloudsync_context *data, const char *savepoint_name) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(savepoint_name);
    // DuckDB does not support SAVEPOINTs and each conn->Query() auto-commits.
    // Each DDL/DML statement is individually atomic; no explicit transactions needed.
    return DBRES_OK;
}

extern "C" int database_commit_savepoint(cloudsync_context *data, const char *savepoint_name) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(savepoint_name);
    return DBRES_OK;
}

extern "C" int database_rollback_savepoint(cloudsync_context *data, const char *savepoint_name) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(savepoint_name);
    return DBRES_OK;
}

extern "C" bool database_in_transaction(cloudsync_context *data) {
    Connection *conn = get_conn(data);
    if (!conn) return false;
    return conn->context->transaction.HasActiveTransaction();
}

extern "C" int database_errcode(cloudsync_context *data) {
    return cloudsync_errcode(data);
}

extern "C" const char *database_errmsg(cloudsync_context *data) {
    return cloudsync_errmsg(data);
}

// MARK: - Prepared statement (VM) operations

extern "C" int databasevm_prepare(cloudsync_context *data, const char *sql, dbvm_t **vm, int flags) {
    UNUSED_PARAMETER(flags);

    *vm = NULL;
    Connection *conn = get_conn(data);
    if (!conn) return cloudsync_set_error(data, "No database connection", DBRES_ERROR);

    try {
        duck_stmt_t *stmt = new duck_stmt_t();
        stmt->sql_text = sql;
        stmt->conn = conn;
        stmt->data = data;
        stmt->executed = false;
        stmt->done = false;
        stmt->chunk_row = 0;
        stmt->nparams = 0;
        memset(stmt->param_set, 0, sizeof(stmt->param_set));

        stmt->prepared = conn->Prepare(sql);
        if (stmt->prepared->HasError()) {
            string err = stmt->prepared->GetError();
            delete stmt;
            return cloudsync_set_error(data, err.c_str(), DBRES_ERROR);
        }

        *vm = (dbvm_t *)stmt;
        return DBRES_OK;
    } catch (std::exception &e) {
        return cloudsync_set_error(data, e.what(), DBRES_ERROR);
    }
}

extern "C" int databasevm_step(dbvm_t *vm) {
    if (!vm) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    if (stmt->done) return DBRES_DONE;

    // Invalidate column cache from previous row
    stmt->col_cache.clear();

    try {
        if (!stmt->executed) {
            // Build parameters vector
            vector<Value> params;
            int expected = stmt->nparams;
            // Also check named_param_map in case nparams wasn't set
            int map_size = (int)stmt->prepared->named_param_map.size();
            if (map_size > expected) expected = map_size;
            for (int i = 0; i < expected; i++) {
                if (stmt->param_set[i]) {
                    params.push_back(stmt->params[i]);
                } else {
                    params.push_back(Value());
                }
            }

            // Re-entrancy check: if we're already inside an Execute on this Connection,
            // we can't nest another Execute. Return cached/default values for read-only
            // version-check statements.
            if (cloudsync_step_depth(stmt->data) > 0) {
                stmt->done = true;
                return DBRES_DONE;
            }

            cloudsync_set_step_depth(stmt->data, cloudsync_step_depth(stmt->data) + 1);
            stmt->result = stmt->prepared->Execute(params, false);
            cloudsync_set_step_depth(stmt->data, cloudsync_step_depth(stmt->data) - 1);
            stmt->executed = true;

            if (stmt->result->HasError()) {
                cloudsync_set_error(stmt->data, stmt->result->GetError().c_str(), DBRES_ERROR);
                stmt->done = true;
                return DBRES_ERROR;
            }

            // Try to fetch first chunk
            if (stmt->result->type == QueryResultType::STREAM_RESULT ||
                stmt->result->type == QueryResultType::MATERIALIZED_RESULT) {
                // Check if this is a DML statement (INSERT/UPDATE/DELETE) that returns
                // a "Count" row rather than actual data. If so, treat it as DONE.
                bool is_dml_count = false;
                if (stmt->result->ColumnCount() == 1) {
                    auto &col_name = stmt->result->names[0];
                    if (col_name == "Count") {
                        is_dml_count = true;
                    }
                }

                stmt->current_chunk = stmt->result->Fetch();
                if (!stmt->current_chunk || stmt->current_chunk->size() == 0 || is_dml_count) {
                    stmt->done = true;
                    return DBRES_DONE;
                }
                stmt->chunk_row = 0;
                return DBRES_ROW;
            }

            stmt->done = true;
            return DBRES_DONE;
        }

        // Already executed, advance to next row
        stmt->chunk_row++;
        if (stmt->current_chunk && stmt->chunk_row < stmt->current_chunk->size()) {
            return DBRES_ROW;
        }

        // Fetch next chunk
        stmt->current_chunk = stmt->result->Fetch();
        if (!stmt->current_chunk || stmt->current_chunk->size() == 0) {
            stmt->done = true;
            return DBRES_DONE;
        }
        stmt->chunk_row = 0;
        return DBRES_ROW;
    } catch (std::exception &e) {
        cloudsync_set_error(stmt->data, e.what(), DBRES_ERROR);
        stmt->done = true;
        return DBRES_ERROR;
    }
}

extern "C" void databasevm_finalize(dbvm_t *vm) {
    if (!vm) return;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    delete stmt;
}

extern "C" void databasevm_reset(dbvm_t *vm) {
    if (!vm) return;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    stmt->result.reset();
    stmt->current_chunk.reset();
    stmt->executed = false;
    stmt->done = false;
    stmt->chunk_row = 0;
    stmt->col_cache.clear();
}

extern "C" void databasevm_clear_bindings(dbvm_t *vm) {
    if (!vm) return;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    memset(stmt->param_set, 0, sizeof(stmt->param_set));
    stmt->nparams = 0;
}

extern "C" const char *databasevm_sql(dbvm_t *vm) {
    if (!vm) return NULL;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    return stmt->sql_text.c_str();
}

// MARK: - Binding

extern "C" int databasevm_bind_blob(dbvm_t *vm, int index, const void *value, uint64_t size) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    stmt->params[index - 1] = Value::BLOB((const_data_ptr_t)value, size);
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

extern "C" int databasevm_bind_double(dbvm_t *vm, int index, double value) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    stmt->params[index - 1] = Value::DOUBLE(value);
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

extern "C" int databasevm_bind_int(dbvm_t *vm, int index, int64_t value) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    stmt->params[index - 1] = Value::BIGINT(value);
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

extern "C" int databasevm_bind_null(dbvm_t *vm, int index) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    stmt->params[index - 1] = Value();
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

extern "C" int databasevm_bind_text(dbvm_t *vm, int index, const char *value, int size) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;

    if (value == NULL) {
        stmt->params[index - 1] = Value();
    } else if (size < 0) {
        stmt->params[index - 1] = Value(string(value));
    } else {
        stmt->params[index - 1] = Value(string(value, size));
    }
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

extern "C" int databasevm_bind_value(dbvm_t *vm, int index, dbvalue_t *value) {
    if (!vm || index < 1 || index > MAX_PARAMS) return DBRES_ERROR;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    duckvalue_t *v = (duckvalue_t *)value;

    if (!v || v->value.IsNull()) {
        return databasevm_bind_null(vm, index);
    }

    // Bind the native DuckDB Value directly
    stmt->params[index - 1] = v->value;
    stmt->param_set[index - 1] = true;
    if (index > stmt->nparams) stmt->nparams = index;
    return DBRES_OK;
}

// MARK: - Column accessors

extern "C" const void *database_column_blob(dbvm_t *vm, int index) {
    if (!vm) return NULL;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return NULL;

    try {
        // Cache Value in statement so returned pointer stays valid until next step/reset
        stmt->col_cache[index] = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        auto &cached = stmt->col_cache[index];
        if (cached.IsNull()) return NULL;

        auto &str = StringValue::Get(cached);
        return str.data();
    } catch (...) {
        return NULL;
    }
}

extern "C" double database_column_double(dbvm_t *vm, int index) {
    if (!vm) return 0.0;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return 0.0;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        if (val.IsNull()) return 0.0;
        return val.GetValue<double>();
    } catch (...) {
        return 0.0;
    }
}

extern "C" int64_t database_column_int(dbvm_t *vm, int index) {
    if (!vm) return 0;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return 0;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        if (val.IsNull()) return 0;
        return val.GetValue<int64_t>();
    } catch (...) {
        return 0;
    }
}

extern "C" const char *database_column_text(dbvm_t *vm, int index) {
    if (!vm) return NULL;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return NULL;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        if (val.IsNull()) return NULL;

        // Convert to VARCHAR so StringValue::Get works for any type (int, double, etc.)
        stmt->col_cache[index] = Value(val.ToString());
        auto &str = StringValue::Get(stmt->col_cache[index]);
        return str.c_str();
    } catch (...) {
        return NULL;
    }
}

extern "C" dbvalue_t *database_column_value(dbvm_t *vm, int index) {
    if (!vm) return NULL;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return NULL;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        return (dbvalue_t *)duckvalue_create(std::move(val));
    } catch (...) {
        return NULL;
    }
}

extern "C" int database_column_bytes(dbvm_t *vm, int index) {
    if (!vm) return 0;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return 0;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        if (val.IsNull()) return 0;

        auto &str = StringValue::Get(val);
        return (int)str.size();
    } catch (...) {
        return 0;
    }
}

extern "C" int database_column_type(dbvm_t *vm, int index) {
    if (!vm) return DBTYPE_NULL;
    duck_stmt_t *stmt = (duck_stmt_t *)vm;
    if (!stmt->current_chunk || stmt->chunk_row >= stmt->current_chunk->size()) return DBTYPE_NULL;

    try {
        auto val = stmt->current_chunk->GetValue(index, stmt->chunk_row);
        if (val.IsNull()) return DBTYPE_NULL;

        return duckvalue_map_type(stmt->result->types[index].id());
    } catch (...) {
        return DBTYPE_NULL;
    }
}

// MARK: - Value accessors (duckvalue_t wrapping native duckdb::Value)

static void duckvalue_ensure_text_cache(duckvalue_t *v) {
    if (v->text_cache) return;
    string str = v->value.ToString();
    v->text_cache = (char *)malloc(str.size() + 1);
    if (v->text_cache) {
        memcpy(v->text_cache, str.c_str(), str.size());
        v->text_cache[str.size()] = '\0';
    }
}

static void duckvalue_ensure_blob_cache(duckvalue_t *v) {
    if (v->blob_cache) return;
    try {
        auto &str = StringValue::Get(v->value);
        v->blob_cache = (char *)malloc(str.size());
        if (v->blob_cache) {
            memcpy(v->blob_cache, str.data(), str.size());
            v->blob_cache_len = (int)str.size();
        }
    } catch (...) {
        v->blob_cache = NULL;
        v->blob_cache_len = 0;
    }
}

extern "C" const void *database_value_blob(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v || v->value.IsNull()) return NULL;
    int dbtype = duckvalue_map_type(v->value.type().id());
    if (dbtype == DBTYPE_BLOB) {
        duckvalue_ensure_blob_cache(v);
        return v->blob_cache;
    }
    if (dbtype == DBTYPE_TEXT) {
        // Like SQLite's sqlite3_value_blob, return raw text bytes
        duckvalue_ensure_text_cache(v);
        return v->text_cache;
    }
    return NULL;
}

extern "C" double database_value_double(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v || v->value.IsNull()) return 0.0;
    try {
        return v->value.GetValue<double>();
    } catch (...) {
        return 0.0;
    }
}

extern "C" int64_t database_value_int(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v || v->value.IsNull()) return 0;
    try {
        return v->value.GetValue<int64_t>();
    } catch (...) {
        return 0;
    }
}

extern "C" const char *database_value_text(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v || v->value.IsNull()) return NULL;
    int dbtype = duckvalue_map_type(v->value.type().id());
    if (dbtype != DBTYPE_TEXT) return NULL;
    duckvalue_ensure_text_cache(v);
    return v->text_cache;
}

extern "C" int database_value_bytes(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v || v->value.IsNull()) return 0;
    try {
        int dbtype = duckvalue_map_type(v->value.type().id());
        if (dbtype == DBTYPE_BLOB) {
            duckvalue_ensure_blob_cache(v);
            return v->blob_cache_len;
        }
        auto &str = StringValue::Get(v->value);
        return (int)str.size();
    } catch (...) {
        return 0;
    }
}

extern "C" int database_value_type(dbvalue_t *value) {
    return duckvalue_dbtype((duckvalue_t *)value);
}

extern "C" void database_value_free(dbvalue_t *value) {
    duckvalue_free((duckvalue_t *)value);
}

extern "C" void *database_value_dup(dbvalue_t *value) {
    duckvalue_t *v = (duckvalue_t *)value;
    if (!v) return NULL;
    return duckvalue_create(v->value);
}

// MARK: - Memory

extern "C" void *dbmem_alloc(uint64_t size) {
    return malloc((size_t)size);
}

extern "C" void *dbmem_zeroalloc(uint64_t size) {
    return calloc(1, (size_t)size);
}

extern "C" void *dbmem_realloc(void *ptr, uint64_t new_size) {
    return realloc(ptr, (size_t)new_size);
}

extern "C" char *dbmem_mprintf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *result = dbmem_vmprintf(format, args);
    va_end(args);
    return result;
}

extern "C" char *dbmem_vmprintf(const char *format, va_list list) {
    va_list args_copy;
    va_copy(args_copy, list);
    int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    if (len < 0) return NULL;

    char *buffer = (char *)malloc(len + 1);
    if (!buffer) return NULL;

    vsnprintf(buffer, len + 1, format, list);
    return buffer;
}

extern "C" void dbmem_free(void *ptr) {
    free(ptr);
}

extern "C" uint64_t dbmem_size(void *ptr) {
    UNUSED_PARAMETER(ptr);
    return 0;  // Not available with standard malloc
}

// MARK: - duckvalue_t implementation

duckvalue_t *duckvalue_create(Value val) {
    duckvalue_t *v = new (std::nothrow) duckvalue_t(std::move(val));
    return v;
}

// MARK: - Payload apply callback (RLS support)

static cloudsync_payload_apply_callback_t g_payload_apply_callback = NULL;

extern "C" cloudsync_payload_apply_callback_t cloudsync_get_payload_apply_callback(void *db) {
    UNUSED_PARAMETER(db);
    return g_payload_apply_callback;
}

extern "C" void cloudsync_set_payload_apply_callback(void *db, cloudsync_payload_apply_callback_t callback) {
    UNUSED_PARAMETER(db);
    g_payload_apply_callback = callback;
}

duckvalue_t *duckvalue_create_null(void) {
    return duckvalue_create(Value());
}

void duckvalue_free(duckvalue_t *v) {
    delete v;
}

int duckvalue_dbtype(duckvalue_t *v) {
    if (!v || v->value.IsNull()) return DBTYPE_NULL;
    return duckvalue_map_type(v->value.type().id());
}
