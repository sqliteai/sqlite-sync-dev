//
//  duckvalue.h
//  cloudsync
//
//  DuckDB-specific dbvalue_t wrapper using native duckdb::Value.
//

#pragma once

#include "duckdb.hpp"

extern "C" {
#include "../database.h"
}

#include <stdlib.h>

// dbvalue_t representation for DuckDB.
// Wraps a native duckdb::Value with caches for database_value_text/blob lifetime.
struct duckvalue_t {
    duckdb::Value   value;
    mutable char   *text_cache;
    mutable char   *blob_cache;
    mutable int     blob_cache_len;

    duckvalue_t() : text_cache(nullptr), blob_cache(nullptr), blob_cache_len(0) {}
    explicit duckvalue_t(duckdb::Value v) : value(std::move(v)), text_cache(nullptr), blob_cache(nullptr), blob_cache_len(0) {}
    ~duckvalue_t() {
        if (text_cache) free(text_cache);
        if (blob_cache) free(blob_cache);
    }

    // Non-copyable (use duckvalue_dup for explicit copies)
    duckvalue_t(const duckvalue_t &) = delete;
    duckvalue_t &operator=(const duckvalue_t &) = delete;
};

duckvalue_t *duckvalue_create(duckdb::Value val);
duckvalue_t *duckvalue_create_null(void);
void duckvalue_free(duckvalue_t *v);
int duckvalue_dbtype(duckvalue_t *v);
int duckvalue_map_type(duckdb::LogicalTypeId type_id);

// Max parameters per statement
#define MAX_PARAMS 32

// DuckDB prepared statement wrapper (shared between database_duckdb.cpp and cloudsync_duckdb.cpp)
struct duck_stmt_t {
    duckdb::shared_ptr<duckdb::PreparedStatement>    prepared;
    duckdb::unique_ptr<duckdb::QueryResult>          result;
    duckdb::unique_ptr<duckdb::DataChunk>            current_chunk;
    duckdb::idx_t                                    chunk_row;
    bool                                             executed;
    bool                                             done;

    std::string                                      sql_text;
    duckdb::Connection                              *conn;
    cloudsync_context                               *data;

    duckdb::Value                                    params[MAX_PARAMS];
    bool                                             param_set[MAX_PARAMS];
    int                                              nparams;

    std::map<int, duckdb::Value>                     col_cache;
};
