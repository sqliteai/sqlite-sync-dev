//
//  cloudsync_duckdb.cpp
//  cloudsync
//
//  DuckDB extension entry point and function registration.
//  Registers all CloudSync SQL functions for DuckDB.
//

#define DUCKDB_EXTENSION_MAIN

#include "cloudsync_duckdb.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckvalue.h"
#include <cinttypes>
#include <mutex>
#include <unordered_map>

// CloudSync headers
#include "../cloudsync.h"

extern "C" {
#include "../database.h"
#include "../dbutils.h"
#include "../pk.h"
#include "../utils.h"
}

using namespace duckdb;

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(X) (void)(X)
#endif

// MARK: - Per-Database State

// Each DatabaseInstance gets its own CloudSync state
struct CloudSyncDatabaseState {
    unique_ptr<Connection> connection;
    cloudsync_context *context = nullptr;
    DatabaseInstance *db_instance = nullptr;

    ~CloudSyncDatabaseState() {
        if (context) {
            cloudsync_context_free(context);
            context = nullptr;
        }
    }
};

// Registry of per-database states
static std::mutex g_state_mutex;
static std::unordered_map<DatabaseInstance *, unique_ptr<CloudSyncDatabaseState>> g_states;

static CloudSyncDatabaseState *InitCloudSyncContext(DatabaseInstance &db);

static CloudSyncDatabaseState *GetDatabaseState(DatabaseInstance *db) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_states.find(db);
    if (it != g_states.end()) return it->second.get();
    return nullptr;
}

// Lazy initialization: create the context on first use
static CloudSyncDatabaseState *GetOrCreateDatabaseState(DatabaseInstance *db) {
    auto *state = GetDatabaseState(db);
    if (state) return state;
    return InitCloudSyncContext(*db);
}

// FunctionData that carries a pointer to the per-database state.
// Attached to every scalar/aggregate/table function via the bind callback.
struct CloudSyncBindData : public FunctionData {
    CloudSyncDatabaseState *db_state;

    explicit CloudSyncBindData(CloudSyncDatabaseState *s) : db_state(s) {}

    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<CloudSyncBindData>(db_state);
    }
    bool Equals(const FunctionData &other) const override {
        return db_state == other.Cast<CloudSyncBindData>().db_state;
    }
};

// Bind callback for scalar functions — looks up (or lazily creates) per-database state
static unique_ptr<FunctionData> CloudSyncScalarBind(ClientContext &context, ScalarFunction &,
                                                     vector<unique_ptr<Expression>> &) {
    auto &db = DatabaseInstance::GetDatabase(context);
    auto *state = GetOrCreateDatabaseState(&db);
    return make_uniq<CloudSyncBindData>(state);
}

// Bind callback for aggregate functions
static unique_ptr<FunctionData> CloudSyncAggregateBind(ClientContext &context, AggregateFunction &,
                                                        vector<unique_ptr<Expression>> &) {
    auto &db = DatabaseInstance::GetDatabase(context);
    auto *state = GetOrCreateDatabaseState(&db);
    return make_uniq<CloudSyncBindData>(state);
}

// Helper: extract CloudSyncDatabaseState from scalar function ExpressionState
static CloudSyncDatabaseState *GetStateFromExpr(ExpressionState &state) {
    auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
    if (!func_expr.bind_info) return nullptr;
    return func_expr.bind_info->Cast<CloudSyncBindData>().db_state;
}

// Helper: extract cloudsync_context from scalar function ExpressionState
static cloudsync_context *GetContextFromExpr(ExpressionState &state) {
    auto *db_state = GetStateFromExpr(state);
    return db_state ? db_state->context : nullptr;
}

// Helper: extract DatabaseInstance from scalar function ExpressionState
static DatabaseInstance *GetDbInstanceFromExpr(ExpressionState &state) {
    auto *db_state = GetStateFromExpr(state);
    return db_state ? db_state->db_instance : nullptr;
}

// MARK: - cloudsync_version()

static void CloudSyncVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
    result.SetValue(0, Value(CLOUDSYNC_VERSION));
}

// MARK: - cloudsync_txn_id()

static void CloudSyncTxnIdFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto *db_state = GetStateFromExpr(state);
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
    if (db_state && db_state->db_instance) {
        auto &db_mgr = db_state->db_instance->GetDatabaseManager();
        auto dbs = db_mgr.GetDatabases();
        for (auto &db : dbs) {
            if (db->IsSystem()) continue;
            auto &txn_mgr = DuckTransactionManager::Get(*db);
            result.SetValue(0, Value::BIGINT((int64_t)txn_mgr.GetLastCommit()));
            return;
        }
    }
    result.SetValue(0, Value::BIGINT(0));
}

// MARK: - cloudsync_uuid()

static void CloudSyncUuidFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    auto count = args.size();
    for (idx_t i = 0; i < count; i++) {
        uint8_t uuid_bytes[UUID_LEN];
        cloudsync_uuid_v7(uuid_bytes);

        char uuid_str[UUID_STR_MAXLEN];
        cloudsync_uuid_v7_stringify(uuid_bytes, uuid_str, true);

        result.SetValue(i, Value(string(uuid_str)));
    }
}

// MARK: - cloudsync_siteid()

static void CloudSyncSiteidFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    UNUSED_PARAMETER(args);

    result.SetVectorType(VectorType::CONSTANT_VECTOR);

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        result.SetValue(0, Value());
        return;
    }

    const void *siteid = cloudsync_siteid(data);
    if (!siteid) {
        result.SetValue(0, Value());
        return;
    }

    result.SetValue(0, Value::BLOB((const_data_ptr_t)siteid, UUID_LEN));
}

// MARK: - cloudsync_db_version()

static void CloudSyncDbVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    UNUSED_PARAMETER(args);

    result.SetVectorType(VectorType::CONSTANT_VECTOR);

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        result.SetValue(0, Value::BIGINT(0));
        return;
    }

    // Force recomputation from actual DB data.
    int rc = cloudsync_dbversion_rerun(data);
    if (rc != 0) {
        throw InvalidInputException("Unable to retrieve db_version: %s", database_errmsg(data));
    }

    int64_t version = cloudsync_dbversion(data);
    result.SetValue(0, Value::BIGINT(version));
}

// MARK: - cloudsync_db_version_next()

static void CloudSyncDbVersionNextFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        throw InvalidInputException("CloudSync not initialized");
    }

    int64_t merging_version = CLOUDSYNC_VALUE_NOTSET;
    if (args.ColumnCount() > 0 && args.size() > 0) {
        auto val = args.GetValue(0, 0);
        if (!val.IsNull()) {
            merging_version = val.GetValue<int64_t>();
        }
    }

    int64_t next_version = cloudsync_dbversion_next(data, merging_version);
    result.SetValue(0, Value::BIGINT(next_version));
}

// MARK: - cloudsync_init(table_name, [algo], [skip_int_pk_check])

static void CloudSyncInitFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto table_val = args.GetValue(0, 0);
    if (table_val.IsNull()) {
        throw InvalidInputException("table_name cannot be NULL");
    }
    string table_name = table_val.ToString();

    const char *algo = NULL;
    string algo_str;
    bool skip_int_pk_check = false;

    if (args.ColumnCount() >= 2) {
        auto algo_val = args.GetValue(1, 0);
        if (!algo_val.IsNull()) {
            algo_str = algo_val.ToString();
            algo = algo_str.c_str();
        }
    }

    if (args.ColumnCount() >= 3) {
        auto skip_val = args.GetValue(2, 0);
        if (!skip_val.IsNull()) {
            skip_int_pk_check = skip_val.GetValue<bool>();
        }
    }

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        throw InvalidInputException("CloudSync not initialized");
    }

    int rc = database_begin_savepoint(data, "cloudsync_init");
    if (rc != DBRES_OK) {
        throw InvalidInputException("Unable to create savepoint: %s", database_errmsg(data));
    }

    // Try to pre-init context to get better error messages
    if (cloudsync_context_init(data) == NULL) {
        const char *err = cloudsync_errmsg(data);
        const char *dberr = database_errmsg(data);
        throw InvalidInputException("Unable to initialize cloudsync context (err=%s dberr=%s)",
                                    err ? err : "null", dberr ? dberr : "null");
    }

    rc = cloudsync_init_table(data, table_name.c_str(), algo, skip_int_pk_check);
    if (rc == DBRES_OK) {
        rc = database_commit_savepoint(data, "cloudsync_init");
        if (rc != DBRES_OK) {
            throw InvalidInputException("Unable to release savepoint: %s", database_errmsg(data));
        }
    } else {
        string err = cloudsync_errmsg(data);
        database_rollback_savepoint(data, "cloudsync_init");
        throw InvalidInputException("%s", err.c_str());
    }

    cloudsync_update_schema_hash(data);
    dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);

    const void *siteid = cloudsync_siteid(data);
    if (siteid) {
        result.SetValue(0, Value::BLOB((const_data_ptr_t)siteid, UUID_LEN));
    } else {
        result.SetValue(0, Value());
    }
}

// MARK: - cloudsync_enable / cloudsync_disable / cloudsync_is_enabled

static void CloudSyncEnableFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto table_val = args.GetValue(0, 0);
    if (table_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_table_context *table = table_lookup(data, table_val.ToString().c_str());
    if (table) table_set_enabled(table, true);

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncDisableFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto table_val = args.GetValue(0, 0);
    if (table_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_table_context *table = table_lookup(data, table_val.ToString().c_str());
    if (table) table_set_enabled(table, false);

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncIsEnabledFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto table_val = args.GetValue(0, 0);
    if (table_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_table_context *table = table_lookup(data, table_val.ToString().c_str());
    bool enabled = (table && table_enabled(table));

    result.SetValue(0, Value::BOOLEAN(enabled));
}

// MARK: - cloudsync_cleanup / cloudsync_terminate

static void CloudSyncCleanupFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto table_val = args.GetValue(0, 0);
    if (table_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    int rc = cloudsync_cleanup(data, table_val.ToString().c_str());
    if (rc != DBRES_OK) {
        throw InvalidInputException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncTerminateFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    UNUSED_PARAMETER(args);

    cloudsync_context *data = GetContextFromExpr(state);
    int rc = cloudsync_terminate(data);

    result.SetValue(0, Value::BOOLEAN(rc == DBRES_OK));
}

// MARK: - cloudsync_set / cloudsync_set_table / cloudsync_set_column

static void CloudSyncSetFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto key_val = args.GetValue(0, 0);
    if (key_val.IsNull()) {
        result.SetValue(0, Value::BOOLEAN(true));
        return;
    }

    string key = key_val.ToString();
    const char *value = NULL;
    string value_str;
    auto val = args.GetValue(1, 0);
    if (!val.IsNull()) {
        value_str = val.ToString();
        value = value_str.c_str();
    }

    cloudsync_context *data = GetContextFromExpr(state);
    dbutils_settings_set_key_value(data, key.c_str(), value);

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncSetTableFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    auto key_val = args.GetValue(1, 0);
    auto val_val = args.GetValue(2, 0);

    string tbl_str = tbl_val.IsNull() ? "" : tbl_val.ToString();
    string key_str = key_val.IsNull() ? "" : key_val.ToString();
    string val_str = val_val.IsNull() ? "" : val_val.ToString();

    cloudsync_context *data = GetContextFromExpr(state);
    dbutils_table_settings_set_key_value(data,
        tbl_val.IsNull() ? NULL : tbl_str.c_str(),
        "*",
        key_val.IsNull() ? NULL : key_str.c_str(),
        val_val.IsNull() ? NULL : val_str.c_str());

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncSetColumnFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    auto col_val = args.GetValue(1, 0);
    auto key_val = args.GetValue(2, 0);
    auto val_val = args.GetValue(3, 0);

    string tbl_str = tbl_val.IsNull() ? "" : tbl_val.ToString();
    string col_str = col_val.IsNull() ? "" : col_val.ToString();
    string key_str = key_val.IsNull() ? "" : key_val.ToString();
    string val_str = val_val.IsNull() ? "" : val_val.ToString();

    cloudsync_context *data = GetContextFromExpr(state);
    dbutils_table_settings_set_key_value(data,
        tbl_val.IsNull() ? NULL : tbl_str.c_str(),
        col_val.IsNull() ? NULL : col_str.c_str(),
        key_val.IsNull() ? NULL : key_str.c_str(),
        val_val.IsNull() ? NULL : val_str.c_str());

    result.SetValue(0, Value::BOOLEAN(true));
}

// MARK: - cloudsync_set_filter / cloudsync_clear_filter

static void CloudSyncSetFilterFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    auto filter_val = args.GetValue(1, 0);
    if (tbl_val.IsNull() || filter_val.IsNull()) {
        throw InvalidInputException("table and filter expression required");
    }

    string tbl = tbl_val.ToString();
    string filter_expr = filter_val.ToString();

    cloudsync_context *data = GetContextFromExpr(state);
    dbutils_table_settings_set_key_value(data, tbl.c_str(), "*", "filter", filter_expr.c_str());

    // Read current algo and recreate triggers (no-op in DuckDB, but keep settings consistent)
    table_algo algo = dbutils_table_settings_get_algo(data, tbl.c_str());
    if (algo == table_algo_none) algo = table_algo_crdt_cls;

    database_delete_triggers(data, tbl.c_str());
    database_create_triggers(data, tbl.c_str(), algo, filter_expr.c_str());

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncClearFilterFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) {
        throw InvalidInputException("table_name cannot be NULL");
    }

    string tbl = tbl_val.ToString();
    cloudsync_context *data = GetContextFromExpr(state);
    dbutils_table_settings_set_key_value(data, tbl.c_str(), "*", "filter", NULL);

    table_algo algo = dbutils_table_settings_get_algo(data, tbl.c_str());
    if (algo == table_algo_none) algo = table_algo_crdt_cls;

    database_delete_triggers(data, tbl.c_str());
    database_create_triggers(data, tbl.c_str(), algo, NULL);

    result.SetValue(0, Value::BOOLEAN(true));
}

// MARK: - cloudsync_set_schema / cloudsync_schema / cloudsync_table_schema

static void CloudSyncSetSchemaFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    const char *schema = NULL;
    string schema_str;
    auto val = args.GetValue(0, 0);
    if (!val.IsNull()) {
        schema_str = val.ToString();
        schema = schema_str.c_str();
    }

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_set_schema(data, schema);

    if (database_internal_table_exists(data, CLOUDSYNC_SETTINGS_NAME)) {
        dbutils_settings_set_key_value(data, "schema", schema);
    }

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncSchemaFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);
    UNUSED_PARAMETER(args);

    result.SetVectorType(VectorType::CONSTANT_VECTOR);

    cloudsync_context *data = GetContextFromExpr(state);
    const char *schema = cloudsync_schema(data);
    if (schema) {
        result.SetValue(0, Value(string(schema)));
    } else {
        result.SetValue(0, Value());
    }
}

static void CloudSyncTableSchemaFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    const char *schema = cloudsync_table_schema(data, tbl_val.ToString().c_str());
    if (schema) {
        result.SetValue(0, Value(string(schema)));
    } else {
        result.SetValue(0, Value());
    }
}

// MARK: - cloudsync_begin_alter / cloudsync_commit_alter

static void CloudSyncBeginAlterFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    int rc = cloudsync_begin_alter(data, tbl_val.ToString().c_str());
    if (rc != DBRES_OK) {
        throw InvalidInputException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::BOOLEAN(true));
}

static void CloudSyncCommitAlterFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");

    cloudsync_context *data = GetContextFromExpr(state);
    int rc = cloudsync_commit_alter(data, tbl_val.ToString().c_str());
    if (rc != DBRES_OK) {
        throw InvalidInputException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::BOOLEAN(true));
}

// MARK: - cloudsync_seq()

static void CloudSyncSeqFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    cloudsync_context *data = GetContextFromExpr(state);

    // Must loop: cloudsync_seq() is used per-row in SQL (e.g. in REKEY queries)
    auto count = args.size();
    for (idx_t i = 0; i < count; i++) {
        int seq = cloudsync_bumpseq(data);
        result.SetValue(i, Value::INTEGER(seq));
    }
}

// MARK: - cloudsync_value_encode(value) — single value pk-encoded (no element count prefix)
// Used internally by the changes view to encode column values.

static void CloudSyncValueEncodeFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto count = args.size();
    for (idx_t row = 0; row < count; row++) {
        auto val = args.GetValue(0, row);
        duckvalue_t *dv = duckvalue_create(val);
        size_t encoded_len = pk_encode_size((dbvalue_t **)&dv, 1, 0, -1);
        char *buf = (char *)cloudsync_memory_alloc(encoded_len);
        pk_encode((dbvalue_t **)&dv, 1, buf, false, &encoded_len, -1);
        duckvalue_free(dv);

        result.SetValue(row, Value::BLOB((const_data_ptr_t)buf, encoded_len));
        cloudsync_memory_free(buf);
    }
}

// MARK: - cloudsync_pk_encode(...)

static void CloudSyncPkEncodeFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    int argc = (int)args.ColumnCount();
    if (argc == 0) {
        throw InvalidInputException("cloudsync_pk_encode requires at least one argument");
    }

    auto count = args.size();
    for (idx_t row = 0; row < count; row++) {
        // Convert all arguments to duckvalue_t
        duckvalue_t **argv = (duckvalue_t **)cloudsync_memory_alloc(argc * sizeof(duckvalue_t *));
        if (!argv) throw InternalException("Out of memory");

        for (int i = 0; i < argc; i++) {
            auto val = args.GetValue(i, row);
            argv[i] = duckvalue_create(val);
        }

        size_t pklen = 0;
        char *encoded = pk_encode_prikey((dbvalue_t **)argv, argc, NULL, &pklen);

        for (int i = 0; i < argc; i++) {
            duckvalue_free(argv[i]);
        }
        cloudsync_memory_free(argv);

        if (!encoded) {
            throw InternalException("Failed to encode primary key");
        }

        result.SetValue(row, Value::BLOB((const_data_ptr_t)encoded, pklen));
        cloudsync_memory_free(encoded);
    }
}

// MARK: - cloudsync_pk_decode(pk, index)

static void CloudSyncPkDecodeFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto count = args.size();
    for (idx_t row = 0; row < count; row++) {
        auto pk_val = args.GetValue(0, row);
        auto idx_val = args.GetValue(1, row);

        if (pk_val.IsNull() || idx_val.IsNull()) {
            result.SetValue(row, Value());
            continue;
        }

        int target_index = idx_val.GetValue<int32_t>();
        if (target_index < 0) {
            result.SetValue(row, Value());
            continue;
        }

        auto &pk_blob = StringValue::Get(pk_val);

        // Decode using callback
        struct decode_ctx {
            int target;
            string result_str;
            bool found;
        } ctx = {target_index, "", false};

        pk_decode_prikey((char *)pk_blob.data(), pk_blob.size(),
            [](void *xdata, int index, int type, int64_t ival, double dval, char *pval) -> int {
                decode_ctx *ctx = (decode_ctx *)xdata;
                if (ctx->found || (index + 1) != ctx->target) return DBRES_OK;

                switch (type) {
                    case DBTYPE_INTEGER:
                        ctx->result_str = std::to_string(ival);
                        break;
                    case DBTYPE_FLOAT:
                        ctx->result_str = std::to_string(dval);
                        break;
                    case DBTYPE_TEXT:
                        ctx->result_str = string(pval, (size_t)ival);
                        break;
                    case DBTYPE_BLOB:
                        ctx->result_str = string(pval, (size_t)ival);
                        break;
                    default:
                        return DBRES_OK;
                }
                ctx->found = true;
                return DBRES_OK;
            }, &ctx);

        if (ctx.found) {
            result.SetValue(row, Value(ctx.result_str));
        } else {
            result.SetValue(row, Value());
        }
    }
}

// MARK: - cloudsync_is_sync(table_name)

static void CloudSyncIsSyncFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    cloudsync_context *data = GetContextFromExpr(state);

    auto count = args.size();
    for (idx_t row = 0; row < count; row++) {
        if (cloudsync_insync(data)) {
            result.SetValue(row, Value::BOOLEAN(true));
            continue;
        }

        auto tbl_val = args.GetValue(0, row);
        if (tbl_val.IsNull()) {
            result.SetValue(row, Value::BOOLEAN(false));
            continue;
        }

        cloudsync_table_context *table = table_lookup(data, tbl_val.ToString().c_str());
        bool is_sync = (table && (table_enabled(table) == 0));
        result.SetValue(row, Value::BOOLEAN(is_sync));
    }
}

// Helper: check if a row identified by PK values matches the table's filter.
// Returns true if there is no filter, or if the row satisfies the filter condition.
static bool RowMatchesFilter(cloudsync_context *data, const char *table_name, DataChunk &args, int pk_start, int pk_count) {
    char fbuf[2048];
    int frc = dbutils_table_settings_get_value(data, table_name, "*", "filter", fbuf, sizeof(fbuf));
    if (frc != DBRES_OK || fbuf[0] == 0) return true; // no filter

    cloudsync_table_context *table = table_lookup(data, table_name);
    if (!table) return true;

    int npks = table_count_pks(table);
    char **pknames = table_pknames(table);
    if (!pknames) {
        char **arr = NULL;
        int ncount = 0;
        if (database_pk_names(data, table_name, &arr, &ncount) == DBRES_OK && arr) {
            table_set_pknames(table, arr);
            pknames = arr;
        }
    }
    if (!pknames) return true;

    // Build: SELECT 1 FROM table WHERE (filter) AND pk1='v1' AND pk2='v2' ... LIMIT 1
    string sql = "SELECT 1 FROM \"" + string(table_name) + "\" WHERE (" + string(fbuf) + ")";
    for (int i = 0; i < npks && i < pk_count; i++) {
        auto val = args.GetValue(pk_start + i, 0);
        if (val.IsNull()) {
            sql += " AND \"" + string(pknames[i]) + "\" IS NULL";
        } else {
            string v = val.ToString();
            string escaped;
            for (auto c : v) { if (c == '\'') escaped += "''"; else escaped += c; }
            sql += " AND \"" + string(pknames[i]) + "\" = '" + escaped + "'";
        }
    }
    sql += " LIMIT 1";

    Connection *conn = (Connection *)cloudsync_db(data);
    if (!conn) return true;

    try {
        auto qresult = conn->Query(sql);
        if (qresult->HasError()) return true;
        auto chunk = qresult->Fetch();
        return (chunk && chunk->size() > 0);
    } catch (...) {
        return true;
    }
}

// MARK: - cloudsync_insert(table_name, pk_values...)

static void CloudSyncInsertFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");
    string table_name = tbl_val.ToString();

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_table_context *table = table_lookup(data, table_name.c_str());

    if (!table) {
        char meta_name[1024];
        snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name.c_str());
        if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
            throw InvalidInputException("Unable to find table %s", table_name.c_str());
        }
        table_algo algo = dbutils_table_settings_get_algo(data, table_name.c_str());
        if (algo == table_algo_none) algo = table_algo_crdt_cls;
        if (!table_add_to_context(data, algo, table_name.c_str())) {
            throw InternalException("Unable to load table context for %s", table_name.c_str());
        }
        table = table_lookup(data, table_name.c_str());
        if (!table) throw InvalidInputException("Unable to find table %s", table_name.c_str());
    }

    int pk_argc = (int)args.ColumnCount() - 1;
    int expected_pks = table_count_pks(table);
    if (pk_argc != expected_pks) {
        throw InvalidInputException("Expected %d primary key values, got %d", expected_pks, pk_argc);
    }

    // Check filter — skip tracking if row doesn't match
    if (!RowMatchesFilter(data, table_name.c_str(), args, 1, pk_argc)) {
        result.SetValue(0, Value::BOOLEAN(true));
        return;
    }

    // Convert PK arguments
    duckvalue_t **pk_argv = (duckvalue_t **)cloudsync_memory_alloc(pk_argc * sizeof(duckvalue_t *));
    if (!pk_argv) throw InternalException("Out of memory");

    for (int i = 0; i < pk_argc; i++) {
        auto val = args.GetValue(i + 1, 0);
        pk_argv[i] = duckvalue_create(val);
    }

    // Encode PK
    char pk_buffer[1024];
    size_t pklen = sizeof(pk_buffer);
    char *pk = pk_encode_prikey((dbvalue_t **)pk_argv, pk_argc, pk_buffer, &pklen);

    for (int i = 0; i < pk_argc; i++) duckvalue_free(pk_argv[i]);
    cloudsync_memory_free(pk_argv);

    if (!pk) throw InternalException("Failed to encode primary key");

    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
    int rc = DBRES_OK;

    // Check if a row with the same primary key already exists
    bool pk_exists = table_pk_exists(table, pk, pklen);

    if (table_count_cols(table) == 0) {
        // PK-only table: sentinel is the only entry
        rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
    } else if (pk_exists) {
        // Re-insert: bump the sentinel
        rc = local_update_sentinel(table, pk, pklen, db_version, cloudsync_bumpseq(data));
    } else {
        // First insert for a table with columns: create sentinel to track row existence
        rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
    }

    if (rc != DBRES_OK) {
        if (pk != pk_buffer) cloudsync_memory_free(pk);
        throw InternalException("%s", database_errmsg(data));
    }

    // Process each non-primary key column for insert or update
    for (int i = 0; i < table_count_cols(table); ++i) {
        rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, i), db_version, cloudsync_bumpseq(data));
        if (rc != DBRES_OK) {
            if (pk != pk_buffer) cloudsync_memory_free(pk);
            throw InternalException("%s", database_errmsg(data));
        }
    }

    if (pk != pk_buffer) cloudsync_memory_free(pk);
    result.SetValue(0, Value::BOOLEAN(true));
}

// MARK: - cloudsync_delete(table_name, pk_values...)

static void CloudSyncDeleteFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto tbl_val = args.GetValue(0, 0);
    if (tbl_val.IsNull()) throw InvalidInputException("table_name cannot be NULL");
    string table_name = tbl_val.ToString();

    cloudsync_context *data = GetContextFromExpr(state);
    cloudsync_table_context *table = table_lookup(data, table_name.c_str());

    if (!table) {
        char meta_name[1024];
        snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name.c_str());
        if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
            throw InvalidInputException("Unable to find table %s", table_name.c_str());
        }
        table_algo algo = dbutils_table_settings_get_algo(data, table_name.c_str());
        if (algo == table_algo_none) algo = table_algo_crdt_cls;
        if (!table_add_to_context(data, algo, table_name.c_str())) {
            throw InternalException("Unable to load table context for %s", table_name.c_str());
        }
        table = table_lookup(data, table_name.c_str());
        if (!table) throw InvalidInputException("Unable to find table %s", table_name.c_str());
    }

    int pk_argc = (int)args.ColumnCount() - 1;
    int expected_pks = table_count_pks(table);
    if (pk_argc != expected_pks) {
        throw InvalidInputException("Expected %d primary key values, got %d", expected_pks, pk_argc);
    }

    duckvalue_t **pk_argv = (duckvalue_t **)cloudsync_memory_alloc(pk_argc * sizeof(duckvalue_t *));
    if (!pk_argv) throw InternalException("Out of memory");
    for (int i = 0; i < pk_argc; i++) {
        pk_argv[i] = duckvalue_create(args.GetValue(i + 1, 0));
    }

    char pk_buffer[1024];
    size_t pklen = sizeof(pk_buffer);
    char *pk = pk_encode_prikey((dbvalue_t **)pk_argv, pk_argc, pk_buffer, &pklen);

    for (int i = 0; i < pk_argc; i++) duckvalue_free(pk_argv[i]);
    cloudsync_memory_free(pk_argv);

    if (!pk) throw InternalException("Failed to encode primary key");

    // Check filter — for deletes, the row is already gone from the user table,
    // so we check whether this PK was ever tracked in metadata. If not (filtered
    // out on insert), skip the delete tracking too.
    {
        char fbuf[2048];
        int frc = dbutils_table_settings_get_value(data, table_name.c_str(), "*", "filter", fbuf, sizeof(fbuf));
        if (frc == DBRES_OK && fbuf[0] != 0) {
            if (!table_pk_exists(table, pk, pklen)) {
                if (pk != pk_buffer) cloudsync_memory_free(pk);
                result.SetValue(0, Value::BOOLEAN(true));
                return;
            }
        }
    }

    int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
    int rc = local_mark_delete_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));
    if (rc == DBRES_OK) {
        rc = local_drop_meta(table, pk, pklen);
    }

    if (pk != pk_buffer) cloudsync_memory_free(pk);

    if (rc != DBRES_OK) {
        throw InternalException("%s", database_errmsg(data));
    }

    result.SetValue(0, Value::BOOLEAN(true));
}

// Forward declaration for DuckDB payload apply callback
static bool duckdb_payload_apply_callback(void **xdata, cloudsync_pk_decode_bind_context *ctx,
                                           void *db, void *vdata, int step, int rc_in);

// MARK: - cloudsync_payload_apply(payload)

static void CloudSyncPayloadApplyFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto payload_val = args.GetValue(0, 0);
    if (payload_val.IsNull()) {
        throw InvalidInputException("payload cannot be NULL");
    }

    auto &payload_blob = StringValue::Get(payload_val);
    int blen = (int)payload_blob.size();

    size_t header_size = 0;
    cloudsync_payload_context_size(&header_size);
    if (blen < (int)header_size) {
        throw InvalidInputException("Invalid payload size");
    }

    cloudsync_context *data = GetContextFromExpr(state);

    // Set callback to bypass SQL INSERT and call merge_insert directly (avoids deadlock)
    cloudsync_set_payload_apply_callback(cloudsync_db(data), duckdb_payload_apply_callback);

    int nrows = 0;
    int rc = cloudsync_payload_apply(data, payload_blob.data(), blen, &nrows);

    cloudsync_set_payload_apply_callback(cloudsync_db(data), nullptr);

    if (rc != DBRES_OK) {
        throw InternalException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::INTEGER(nrows));
}

// MARK: - cloudsync_payload_save(path)

static void CloudSyncPayloadSaveFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto path_val = args.GetValue(0, 0);
    if (path_val.IsNull()) {
        throw InvalidInputException("file path cannot be NULL");
    }
    string payload_path = path_val.ToString();

    auto *db_state = GetStateFromExpr(state);
    if (!db_state || !db_state->context) {
        throw InvalidInputException("CloudSync not initialized");
    }
    cloudsync_context *data = db_state->context;
    DatabaseInstance *db_instance = db_state->db_instance;

    // Retrieve current send_dbversion and send_seq
    int db_version = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_SEND_DBVERSION);
    if (db_version < 0) {
        throw InternalException("Unable to retrieve send_dbversion");
    }
    int seq = dbutils_settings_get_int_value(data, CLOUDSYNC_KEY_SEND_SEQ);
    if (seq < 0) {
        throw InternalException("Unable to retrieve send_seq");
    }

    // Build the payload query (same as cloudsync_payload_get but executed on a separate connection)
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "WITH max_db_version AS (SELECT MAX(db_version) AS max_db_version FROM cloudsync_changes WHERE site_id=cloudsync_siteid()) "
        "SELECT * FROM (SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload, "
        "MAX(max_db_version) AS max_db_version, MAX(CASE WHEN db_version = max_db_version THEN seq ELSE 0 END) "
        "FROM cloudsync_changes, max_db_version WHERE site_id=cloudsync_siteid() AND (db_version>%d OR (db_version=%d AND seq>%d))) "
        "WHERE payload IS NOT NULL", db_version, db_version, seq);

    // Use a separate connection to avoid deadlock (we're inside a scalar function)
    Connection query_conn(*db_instance);
    auto qresult = query_conn.Query(sql);
    if (qresult->HasError()) {
        throw InternalException("Unable to retrieve changes in cloudsync_payload_save (%s)", qresult->GetError().c_str());
    }

    auto chunk = qresult->Fetch();
    if (!chunk || chunk->size() == 0) {
        // No changes to save
        result.SetValue(0, Value::BIGINT(0));
        return;
    }

    // Extract payload blob, new_db_version, new_seq
    auto val0 = chunk->GetValue(0, 0);
    if (val0.IsNull()) {
        result.SetValue(0, Value::BIGINT(0));
        return;
    }
    auto &blob_str = StringValue::Get(val0);
    int blob_size = (int)blob_str.size();

    int64_t new_db_version = 0, new_seq = 0;
    if (chunk->ColumnCount() > 1) {
        auto v1 = chunk->GetValue(1, 0);
        if (!v1.IsNull()) new_db_version = v1.GetValue<int64_t>();
    }
    if (chunk->ColumnCount() > 2) {
        auto v2 = chunk->GetValue(2, 0);
        if (!v2.IsNull()) new_seq = v2.GetValue<int64_t>();
    }

    // Delete existing file, write payload
    cloudsync_file_delete(payload_path.c_str());
    bool written = cloudsync_file_write(payload_path.c_str(), blob_str.data(), (size_t)blob_size);
    if (!written) {
        throw InternalException("Unable to write payload to file path");
    }

    // Update send_dbversion and send_seq
    char buf[256];
    if (new_db_version != db_version) {
        snprintf(buf, sizeof(buf), "%" PRId64, new_db_version);
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_DBVERSION, buf);
    }
    if (new_seq != seq) {
        snprintf(buf, sizeof(buf), "%" PRId64, new_seq);
        dbutils_settings_set_key_value(data, CLOUDSYNC_KEY_SEND_SEQ, buf);
    }

    result.SetValue(0, Value::BIGINT((int64_t)blob_size));
}

// MARK: - cloudsync_payload_load(path)
// DuckDB payload apply callback: intercepts WILL_APPLY to call merge_insert directly,
// bypassing the SQL route which would deadlock on the same connection.
static bool duckdb_payload_apply_callback(void **xdata, cloudsync_pk_decode_bind_context *ctx,
                                           void *db, void *vdata, int step, int rc_in) {
    if (step != CLOUDSYNC_PAYLOAD_APPLY_WILL_APPLY) return true;

    cloudsync_context *data = (cloudsync_context *)vdata;

    int64_t tbl_len = 0;
    char *tbl_raw = cloudsync_pk_context_tbl(ctx, &tbl_len);

    // tbl_raw is not null-terminated; make a copy
    char *tbl = (char *)alloca(tbl_len + 1);
    memcpy(tbl, tbl_raw, tbl_len);
    tbl[tbl_len] = '\0';

    cloudsync_table_context *table = table_lookup(data, tbl);
    if (!table) return false;

    int64_t pk_len = 0;
    void *pk = cloudsync_pk_context_pk(ctx, &pk_len);

    int64_t col_name_len = 0;
    char *col_name_raw = cloudsync_pk_context_colname(ctx, &col_name_len);
    // col_name_raw may not be null-terminated
    char *col_name = nullptr;
    if (col_name_raw && col_name_len > 0) {
        col_name = (char *)alloca(col_name_len + 1);
        memcpy(col_name, col_name_raw, col_name_len);
        col_name[col_name_len] = '\0';
    }
    const char *insert_name = col_name ? col_name : CLOUDSYNC_TOMBSTONE_VALUE;

    // Get col_value from the bound vm parameter at position $4 (index 3, 0-based).
    // The value is pk-encoded (type byte + data) stored as a BLOB in the changes view.
    // We need to decode it to the native DuckDB Value type.
    duck_stmt_t *stmt = (duck_stmt_t *)cloudsync_pk_context_vm(ctx);
    duckvalue_t *dv = nullptr;
    auto &param_val = stmt->params[3];
    if (!param_val.IsNull()) {
        auto &blob_str = StringValue::Get(param_val);
        if (!blob_str.empty()) {
            // pk_decode the single value
            size_t seek = 0;
            struct DecodeResult { Value val; bool decoded; } dr = {Value(), false};
            pk_decode((char *)blob_str.data(), blob_str.size(), 1, &seek, -1,
                [](void *xdata, int index, int type, int64_t ival, double dval, char *pval) -> int {
                    auto *r = (DecodeResult *)xdata;
                    switch (type) {
                        case DBTYPE_INTEGER:
                            r->val = Value::BIGINT(ival);
                            break;
                        case DBTYPE_FLOAT:
                            r->val = Value::DOUBLE(dval);
                            break;
                        case DBTYPE_TEXT:
                            r->val = Value(string(pval, ival));
                            break;
                        case DBTYPE_BLOB:
                            r->val = Value::BLOB((const_data_ptr_t)pval, ival);
                            break;
                        case DBTYPE_NULL:
                            r->val = Value();
                            break;
                        default:
                            return DBRES_ERROR;
                    }
                    r->decoded = true;
                    return DBRES_OK;
                }, &dr);
            if (dr.decoded) {
                dv = duckvalue_create(dr.val);
            }
        }
    }
    if (!dv) {
        dv = duckvalue_create_null();
    }
    dbvalue_t *col_value = (dbvalue_t *)dv;

    int64_t col_version = cloudsync_pk_context_colversion(ctx);
    int64_t db_version = cloudsync_pk_context_dbversion(ctx);
    int64_t cl = cloudsync_pk_context_cl(ctx);
    int64_t seq = cloudsync_pk_context_seq(ctx);
    int64_t site_id_len = 0;
    const char *site_id = (const char *)cloudsync_pk_context_siteid(ctx, &site_id_len);

    int64_t rowid = 0;
    int rc;
    if (table_algo_isgos(table)) {
        rc = merge_insert_col(data, table, (const char *)pk, (int)pk_len, insert_name, col_value,
                              col_version, db_version,
                              site_id, (int)site_id_len,
                              seq, &rowid);
    } else {
        rc = merge_insert(data, table, (const char *)pk, (int)pk_len, cl, insert_name, col_value,
                          col_version, db_version,
                          site_id, (int)site_id_len,
                          seq, &rowid);
    }

    duckvalue_free(dv);

    // Return false so databasevm_step is skipped (we already did the merge)
    return false;
}

static void CloudSyncPayloadLoadFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    auto path_val = args.GetValue(0, 0);
    if (path_val.IsNull()) {
        throw InvalidInputException("file path cannot be NULL");
    }
    string path = path_val.ToString();

    int64_t payload_size = 0;
    char *payload = cloudsync_file_read(path.c_str(), &payload_size);
    if (!payload) {
        if (payload_size < 0) {
            throw InternalException("Unable to read payload from file path");
        }
        result.SetValue(0, Value::INTEGER(0));
        return;
    }

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        cloudsync_memory_free(payload);
        throw InvalidInputException("CloudSync not initialized");
    }

    // Set callback to bypass SQL INSERT and call merge_insert directly
    cloudsync_set_payload_apply_callback(cloudsync_db(data), duckdb_payload_apply_callback);

    int nrows = 0;
    int rc = cloudsync_payload_apply(data, payload, (int)payload_size, &nrows);
    cloudsync_memory_free(payload);

    // Clear callback
    cloudsync_set_payload_apply_callback(cloudsync_db(data), nullptr);

    if (rc != DBRES_OK) {
        throw InternalException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::INTEGER(nrows));
}

// MARK: - cloudsync_merge_insert(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
// Internal function used by cloudsync_payload_apply to route INSERTs through merge_insert
// (DuckDB has no virtual table xUpdate, so we use a scalar function instead)

static void CloudSyncMergeInsertFun(DataChunk &args, ExpressionState &state, Vector &result) {
    UNUSED_PARAMETER(state);

    cloudsync_context *data = GetContextFromExpr(state);
    if (!data) {
        throw InvalidInputException("CloudSync not initialized");
    }

    // argv[0] -> table name (TEXT)
    auto tbl_val = args.GetValue(0, 0);
    string tbl_str = tbl_val.ToString();
    const char *insert_tbl = tbl_str.c_str();

    cloudsync_table_context *table = table_lookup(data, insert_tbl);
    if (!table) {
        throw InvalidInputException("Unable to find table %s", insert_tbl);
    }

    // argv[1] -> primary key (BLOB)
    auto pk_val = args.GetValue(1, 0);
    auto &pk_blob = StringValue::Get(pk_val);
    const char *insert_pk = pk_blob.data();
    int insert_pk_len = (int)pk_blob.size();

    // argv[2] -> column name (TEXT or NULL if sentinel)
    auto col_name_val = args.GetValue(2, 0);
    const char *insert_name = CLOUDSYNC_TOMBSTONE_VALUE;
    string col_name_str;
    if (!col_name_val.IsNull()) {
        col_name_str = col_name_val.ToString();
        insert_name = col_name_str.c_str();
    }

    // argv[3] -> column value (ANY) — wrap as duckvalue_t
    auto col_value_val = args.GetValue(3, 0);
    duckvalue_t *dv = duckvalue_create(col_value_val);
    dbvalue_t *insert_value = (dbvalue_t *)dv;

    // argv[4..8] -> col_version, db_version, site_id, cl, seq
    int64_t insert_col_version = args.GetValue(4, 0).GetValue<int64_t>();
    int64_t insert_db_version = args.GetValue(5, 0).GetValue<int64_t>();

    auto site_val = args.GetValue(6, 0);
    auto &site_blob = StringValue::Get(site_val);
    const char *insert_site_id = site_blob.data();
    int insert_site_id_len = (int)site_blob.size();

    int64_t insert_cl = args.GetValue(7, 0).GetValue<int64_t>();
    int64_t insert_seq = args.GetValue(8, 0).GetValue<int64_t>();

    int64_t rowid = 0;
    int rc;
    if (table_algo_isgos(table)) {
        rc = merge_insert_col(data, table, insert_pk, insert_pk_len, insert_name, insert_value,
                              insert_col_version, insert_db_version, insert_site_id, insert_site_id_len,
                              insert_seq, &rowid);
    } else {
        rc = merge_insert(data, table, insert_pk, insert_pk_len, insert_cl, insert_name, insert_value,
                          insert_col_version, insert_db_version, insert_site_id, insert_site_id_len,
                          insert_seq, &rowid);
    }

    duckvalue_free(dv);

    if (rc != DBRES_OK) {
        throw InternalException("%s", cloudsync_errmsg(data));
    }

    result.SetValue(0, Value::BIGINT(rowid));
}

// MARK: - cloudsync_col_value(table_name, col_name, pk)

static void CloudSyncColValueFun(DataChunk &args, ExpressionState &state, Vector &result) {
    cloudsync_context *data = GetContextFromExpr(state);
    auto row_count = args.size();

    for (idx_t i = 0; i < row_count; i++) {
        auto tbl_val = args.GetValue(0, i);
        auto col_val = args.GetValue(1, i);
        auto pk_val = args.GetValue(2, i);

        if (tbl_val.IsNull() || col_val.IsNull() || pk_val.IsNull()) {
            throw InvalidInputException("cloudsync_col_value arguments cannot be NULL");
        }

        string table_name = tbl_val.ToString();
        string col_name = col_val.ToString();

        if (col_name == CLOUDSYNC_TOMBSTONE_VALUE) {
            result.SetValue(i, Value());
            continue;
        }

        cloudsync_table_context *table = table_lookup(data, table_name.c_str());
        if (!table) {
            throw InvalidInputException("Unable to find table %s", table_name.c_str());
        }

        bool persistent = false;
        dbvm_t *vm = table_column_lookup(table, col_name.c_str(), false, NULL);
        if (vm) {
            persistent = true;
        } else {
            vm = cloudsync_colvalue_stmt(data, table_name.c_str(), &persistent);
        }
        if (!vm) {
            throw InvalidInputException("Unable to find column value statement for %s.%s", table_name.c_str(), col_name.c_str());
        }

        auto &pk_blob = StringValue::Get(pk_val);
        int count = pk_decode_prikey((char *)pk_blob.data(), pk_blob.size(), pk_decode_bind_callback, (void *)vm);
        if (count <= 0) {
            throw InvalidInputException("Unable to decode primary key");
        }

        int rc = databasevm_step(vm);
        if (rc == DBRES_ROW) {
            // Return the raw column value (like SQLite's sqlite3_result_value)
            const char *text = (const char *)database_column_text(vm, 0);
            if (text) {
                result.SetValue(i, Value(string(text)));
            } else {
                result.SetValue(i, Value());
            }
        } else {
            result.SetValue(i, Value());
        }

        databasevm_reset(vm);
        databasevm_clear_bindings(vm);
    }
}

// MARK: - cloudsync_changes_select table function

// Bind data: stores the parameters passed to cloudsync_changes_select()
struct ChangesSelectBindData : public TableFunctionData {
    int64_t min_db_version = 0;
    bool has_site_filter = false;
    string filter_site_id;   // raw blob bytes
    CloudSyncDatabaseState *db_state = nullptr;
};

// Global state: holds materialized result rows
struct ChangesSelectRow {
    string tbl;
    string pk;           // raw blob bytes
    string col_name;
    string col_value;    // raw blob bytes
    bool col_value_null;
    int64_t col_version;
    int64_t db_version;
    string site_id;      // raw blob bytes
    bool site_id_null;
    int64_t cl;
    int64_t seq;
};

struct ChangesSelectGlobalState : public GlobalTableFunctionState {
    vector<ChangesSelectRow> rows;
    idx_t current_row = 0;
    bool done = false;
};

// Build the dynamic UNION ALL SQL for DuckDB (similar to SQLite's vtab_build_changes_sql)
// Uses direct JOINs to user tables for col_value to avoid cross-connection deadlocks.
// The col_value is produced via cloudsync_value_encode(col) which pk-encodes a single
// value without the element count prefix, matching the format expected by the payload encoder.
static string BuildChangesSelectSQL(cloudsync_context *data) {
    int ntables = cloudsync_table_count(data);
    if (ntables <= 0) return "";

    string union_sql;
    int found = 0;
    for (int i = 0; i < ntables; i++) {
        cloudsync_table_context *table = cloudsync_table_at(data, i);
        if (!table) continue;
        const char *base_name = table_name(table);
        const char *meta_ref = table_metaref(table);
        if (!base_name || !meta_ref) continue;

        // Build CASE expression for col_value
        int ncols = table_count_cols(table);
        string col_value_expr;
        if (ncols > 0) {
            col_value_expr = "CASE t1.col_name ";
            for (int c = 0; c < ncols; c++) {
                const char *cn = table_colname(table, c);
                if (!cn) continue;
                col_value_expr += "WHEN '" + string(cn) + "' THEN cloudsync_value_encode(t_user.\"" + string(cn) + "\") ";
            }
            col_value_expr += "WHEN '" CLOUDSYNC_TOMBSTONE_VALUE "' THEN cloudsync_value_encode(NULL) ";
            col_value_expr += "ELSE NULL END";
        } else {
            col_value_expr = "cloudsync_value_encode(NULL)";
        }

        // Build JOIN condition on PKs
        int npks = table_count_pks(table);
        char **pknames = table_pknames(table);
        // Lazily populate pk_name if not yet set (deferred from table_add_to_context
        // because that can run inside database_exec_callback during settings load,
        // where issuing another query on the same connection would crash).
        if (!pknames && npks > 0) {
            char **arr = NULL;
            int pk_count = 0;
            if (database_pk_names(data, base_name, &arr, &pk_count) == DBRES_OK && arr) {
                table_set_pknames(table, arr);
                pknames = arr;
            }
        }
        if (!pknames) continue;
        string join_cond;
        for (int p = 0; p < npks; p++) {
            if (p > 0) join_cond += " AND ";
            join_cond += "t_user.\"" + string(pknames[p]) + "\" = cloudsync_pk_decode(t1.pk, " + to_string(p + 1) + ")";
        }

        if (found > 0) union_sql += " UNION ALL ";
        union_sql += "SELECT ";
        union_sql += "'" + string(base_name) + "' AS tbl, ";
        union_sql += "t1.pk AS pk, ";
        union_sql += "t1.col_name AS col_name, ";
        union_sql += col_value_expr + " AS col_value, ";
        union_sql += "t1.col_version AS col_version, ";
        union_sql += "t1.db_version AS db_version, ";
        union_sql += "site_tbl.site_id AS site_id, ";
        union_sql += "COALESCE(t2.col_version, 1) AS cl, ";
        union_sql += "t1.seq AS seq ";
        union_sql += "FROM " + string(meta_ref) + " AS t1 ";
        union_sql += "LEFT JOIN \"" + string(base_name) + "\" AS t_user ON " + join_cond + " ";
        union_sql += "LEFT JOIN cloudsync_site_id AS site_tbl ON t1.site_id = site_tbl.id ";
        union_sql += "LEFT JOIN " + string(meta_ref) + " AS t2 ON t1.pk = t2.pk AND t2.col_name = '" CLOUDSYNC_TOMBSTONE_VALUE "' ";
        found++;
    }

    if (found == 0) return "";

    return "SELECT * FROM (SELECT tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq FROM (" + union_sql + ")) AS _cs_changes WHERE col_value IS DISTINCT FROM '" CLOUDSYNC_RLS_RESTRICTED_VALUE "'";
}

static unique_ptr<FunctionData> ChangesSelectBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
    // Return columns: tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq
    names = {"tbl", "pk", "col_name", "col_value", "col_version", "db_version", "site_id", "cl", "seq"};
    return_types = {
        LogicalType::VARCHAR,   // tbl
        LogicalType::BLOB,      // pk
        LogicalType::VARCHAR,   // col_name
        LogicalType::BLOB,      // col_value
        LogicalType::BIGINT,    // col_version
        LogicalType::BIGINT,    // db_version
        LogicalType::BLOB,      // site_id
        LogicalType::BIGINT,    // cl
        LogicalType::BIGINT     // seq
    };

    auto bind_data = make_uniq<ChangesSelectBindData>();

    // Store per-database state
    auto &db = DatabaseInstance::GetDatabase(context);
    bind_data->db_state = GetDatabaseState(&db);
    // Parse parameters: (min_db_version BIGINT DEFAULT 0, filter_site_id BLOB DEFAULT NULL)
    if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
        bind_data->min_db_version = input.inputs[0].GetValue<int64_t>();
    }
    if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
        bind_data->has_site_filter = true;
        bind_data->filter_site_id = StringValue::Get(input.inputs[1]);
    }

    return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ChangesSelectInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<ChangesSelectBindData>();
    auto state = make_uniq<ChangesSelectGlobalState>();

    cloudsync_context *data = bind_data.db_state ? bind_data.db_state->context : nullptr;
    if (!data) {
        state->done = true;
        return std::move(state);
    }

    string base_sql = BuildChangesSelectSQL(data);
    if (base_sql.empty()) {
        state->done = true;
        return std::move(state);
    }

    // Build full query with filters
    string sql = base_sql + " AND db_version > " + to_string(bind_data.min_db_version);
    sql += " ORDER BY db_version, seq ASC";

    // Use a separate connection to avoid deadlock (we're inside a table function scan)
    auto &db = DatabaseInstance::GetDatabase(context);
    Connection query_conn(db);
    auto result = query_conn.Query(sql);

    if (result->HasError()) {
        state->done = true;
        return std::move(state);
    }
    // Materialize all rows, applying site_id filter if provided
    while (true) {
        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) break;

        for (idx_t r = 0; r < chunk->size(); r++) {
            ChangesSelectRow row;
            // tbl (VARCHAR)
            auto v0 = chunk->GetValue(0, r);
            row.tbl = v0.IsNull() ? "" : v0.ToString();
            // pk (BLOB)
            auto v1 = chunk->GetValue(1, r);
            if (!v1.IsNull()) row.pk = StringValue::Get(v1);
            // col_name (VARCHAR)
            auto v2 = chunk->GetValue(2, r);
            row.col_name = v2.IsNull() ? "" : v2.ToString();
            // col_value (BLOB)
            auto v3 = chunk->GetValue(3, r);
            row.col_value_null = v3.IsNull();
            if (!v3.IsNull()) row.col_value = StringValue::Get(v3);
            // col_version (BIGINT)
            row.col_version = chunk->GetValue(4, r).GetValue<int64_t>();
            // db_version (BIGINT)
            row.db_version = chunk->GetValue(5, r).GetValue<int64_t>();
            // site_id (BLOB)
            auto v6 = chunk->GetValue(6, r);
            row.site_id_null = v6.IsNull();
            if (!v6.IsNull()) row.site_id = StringValue::Get(v6);
            // cl (BIGINT)
            row.cl = chunk->GetValue(7, r).GetValue<int64_t>();
            // seq (BIGINT)
            row.seq = chunk->GetValue(8, r).GetValue<int64_t>();

            // Apply site_id filter: only include rows matching the given site_id
            if (bind_data.has_site_filter) {
                if (row.site_id_null || row.site_id != bind_data.filter_site_id) {
                    continue;  // skip rows not from the requested site
                }
            }

            state->rows.push_back(std::move(row));
        }
    }

    return std::move(state);
}

static void ChangesSelectFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<ChangesSelectGlobalState>();
    if (state.done || state.current_row >= state.rows.size()) {
        output.SetCardinality(0);
        return;
    }

    idx_t count = 0;
    while (count < STANDARD_VECTOR_SIZE && state.current_row < state.rows.size()) {
        auto &row = state.rows[state.current_row++];

        output.SetValue(0, count, Value(row.tbl));
        output.SetValue(1, count, Value::BLOB((const_data_ptr_t)row.pk.data(), row.pk.size()));
        output.SetValue(2, count, Value(row.col_name));
        output.SetValue(3, count, row.col_value_null ? Value() : Value::BLOB((const_data_ptr_t)row.col_value.data(), row.col_value.size()));
        output.SetValue(4, count, Value::BIGINT(row.col_version));
        output.SetValue(5, count, Value::BIGINT(row.db_version));
        output.SetValue(6, count, row.site_id_null ? Value() : Value::BLOB((const_data_ptr_t)row.site_id.data(), row.site_id.size()));
        output.SetValue(7, count, Value::BIGINT(row.cl));
        output.SetValue(8, count, Value::BIGINT(row.seq));

        count++;
    }
    output.SetCardinality(count);
}

// MARK: - cloudsync_payload_encode aggregate

struct PayloadEncodeState {
    cloudsync_payload_context *payload;
};

static void PayloadEncodeInit(const AggregateFunction &, data_ptr_t state_p) {
    auto &state = *reinterpret_cast<PayloadEncodeState *>(state_p);
    state.payload = nullptr;
}

static void PayloadEncodeUpdate(Vector inputs[], AggregateInputData &aggr_input, idx_t input_count, Vector &states, idx_t count) {
    UnifiedVectorFormat sdata;
    states.ToUnifiedFormat(count, sdata);
    auto state_ptrs = reinterpret_cast<PayloadEncodeState **>(sdata.data);

    cloudsync_context *data = aggr_input.bind_data ? aggr_input.bind_data->Cast<CloudSyncBindData>().db_state->context : nullptr;
    if (!data) throw InternalException("CloudSync not initialized");

    for (idx_t i = 0; i < count; i++) {
        auto sidx = sdata.sel->get_index(i);
        auto &state = *state_ptrs[sidx];

        // Allocate payload on first row
        if (!state.payload) {
            size_t ctx_size = cloudsync_payload_context_size(nullptr);
            state.payload = (cloudsync_payload_context *)cloudsync_memory_alloc(ctx_size);
            if (!state.payload) throw InternalException("Out of memory");
            memset(state.payload, 0, ctx_size);
        }

        // Convert all input columns for this row to duckvalue_t
        int argc = (int)input_count;
        duckvalue_t **argv = (duckvalue_t **)cloudsync_memory_alloc(argc * sizeof(duckvalue_t *));
        if (!argv) throw InternalException("Out of memory");

        for (int c = 0; c < argc; c++) {
            auto val = inputs[c].GetValue(i);
            argv[c] = duckvalue_create(val);
        }

        int rc = cloudsync_payload_encode_step(state.payload, data, argc, (dbvalue_t **)argv);

        for (int c = 0; c < argc; c++) {
            duckvalue_free(argv[c]);
        }
        cloudsync_memory_free(argv);

        if (rc != DBRES_OK) {
            throw InternalException("cloudsync_payload_encode_step failed: %s", cloudsync_errmsg(data));
        }
    }
}

static void PayloadEncodeCombine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
    auto src_ptrs = FlatVector::GetData<PayloadEncodeState *>(source);
    auto tgt_ptrs = FlatVector::GetData<PayloadEncodeState *>(target);
    for (idx_t i = 0; i < count; i++) {
        auto &src = *src_ptrs[i];
        auto &tgt = *tgt_ptrs[i];
        if (src.payload && tgt.payload) {
            int rc = cloudsync_payload_encode_combine(tgt.payload, src.payload);
            if (rc != DBRES_OK) {
                throw InternalException("Failed to combine payload states");
            }
        } else if (src.payload && !tgt.payload) {
            tgt.payload = src.payload;
            src.payload = nullptr;
        }
    }
}

static void PayloadEncodeFinalize(Vector &states, AggregateInputData &aggr_input, Vector &result, idx_t count, idx_t offset) {
    auto state_ptrs = FlatVector::GetData<PayloadEncodeState *>(states);
    cloudsync_context *data = aggr_input.bind_data ? aggr_input.bind_data->Cast<CloudSyncBindData>().db_state->context : nullptr;

    for (idx_t i = 0; i < count; i++) {
        auto &state = *state_ptrs[i];
        idx_t ridx = i + offset;

        if (!state.payload) {
            FlatVector::SetNull(result, ridx, true);
            continue;
        }

        int rc = cloudsync_payload_encode_final(state.payload, data);
        if (rc != DBRES_OK) {
            throw InternalException("cloudsync_payload_encode_final failed: %s", cloudsync_errmsg(data));
        }

        int64_t blob_size = 0;
        char *blob = cloudsync_payload_blob(state.payload, &blob_size, nullptr);
        if (!blob) {
            FlatVector::SetNull(result, ridx, true);
            continue;
        }

        result.SetValue(ridx, Value::BLOB((const_data_ptr_t)blob, (idx_t)blob_size));
        cloudsync_memory_free(blob);
    }
}

static void PayloadEncodeDestructor(Vector &states, AggregateInputData &, idx_t count) {
    auto state_ptrs = FlatVector::GetData<PayloadEncodeState *>(states);
    for (idx_t i = 0; i < count; i++) {
        auto &state = *state_ptrs[i];
        if (state.payload) {
            cloudsync_memory_free(state.payload);
            state.payload = nullptr;
        }
    }
}

// MARK: - cloudsync_update aggregate

struct UpdateAggState {
    duckvalue_t  *table_name;
    duckvalue_t **new_values;
    duckvalue_t **old_values;
    int           count;
    int           capacity;
};

static void UpdateAggInit(const AggregateFunction &, data_ptr_t state_p) {
    auto &state = *reinterpret_cast<UpdateAggState *>(state_p);
    state.table_name = nullptr;
    state.new_values = nullptr;
    state.old_values = nullptr;
    state.count = 0;
    state.capacity = 0;
}

static void UpdateAggUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states, idx_t count) {
    // inputs: [table_name (VARCHAR), new_value (ANY), old_value (ANY)]
    if (input_count != 3) {
        throw InvalidInputException("cloudsync_update requires exactly 3 arguments: table_name, new_value, old_value");
    }

    UnifiedVectorFormat sdata;
    states.ToUnifiedFormat(count, sdata);
    auto state_ptrs = reinterpret_cast<UpdateAggState **>(sdata.data);

    for (idx_t i = 0; i < count; i++) {
        auto sidx = sdata.sel->get_index(i);
        auto &state = *state_ptrs[sidx];

        auto tbl_val = inputs[0].GetValue(i);
        auto new_val = inputs[1].GetValue(i);
        auto old_val = inputs[2].GetValue(i);

        // Grow arrays if needed
        if (state.count >= state.capacity) {
            int newcap = state.capacity ? state.capacity * 2 : 128;
            auto new_arr = (duckvalue_t **)cloudsync_memory_alloc(newcap * sizeof(duckvalue_t *));
            auto old_arr = (duckvalue_t **)cloudsync_memory_alloc(newcap * sizeof(duckvalue_t *));
            if (!new_arr || !old_arr) throw InternalException("Out of memory");

            if (state.count > 0) {
                memcpy(new_arr, state.new_values, state.count * sizeof(duckvalue_t *));
                memcpy(old_arr, state.old_values, state.count * sizeof(duckvalue_t *));
            }
            if (state.new_values) cloudsync_memory_free(state.new_values);
            if (state.old_values) cloudsync_memory_free(state.old_values);
            state.new_values = new_arr;
            state.old_values = old_arr;
            state.capacity = newcap;
        }

        // Store table_name on first call
        if (!state.table_name) {
            state.table_name = duckvalue_create(tbl_val);
        }

        state.new_values[state.count] = duckvalue_create(new_val);
        state.old_values[state.count] = duckvalue_create(old_val);
        state.count++;
    }
}

static void UpdateAggCombine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
    auto src_ptrs = FlatVector::GetData<UpdateAggState *>(source);
    auto tgt_ptrs = FlatVector::GetData<UpdateAggState *>(target);

    for (idx_t i = 0; i < count; i++) {
        auto &src = *src_ptrs[i];
        auto &tgt = *tgt_ptrs[i];

        if (src.count == 0) continue;

        // Take table_name from source if target doesn't have one
        if (!tgt.table_name && src.table_name) {
            tgt.table_name = src.table_name;
            src.table_name = nullptr;
        }

        // Grow target arrays to fit combined data
        int new_count = tgt.count + src.count;
        if (new_count > tgt.capacity) {
            int newcap = tgt.capacity ? tgt.capacity : 128;
            while (newcap < new_count) newcap *= 2;
            auto new_arr = (duckvalue_t **)cloudsync_memory_alloc(newcap * sizeof(duckvalue_t *));
            auto old_arr = (duckvalue_t **)cloudsync_memory_alloc(newcap * sizeof(duckvalue_t *));
            if (!new_arr || !old_arr) throw InternalException("Out of memory");

            if (tgt.count > 0) {
                memcpy(new_arr, tgt.new_values, tgt.count * sizeof(duckvalue_t *));
                memcpy(old_arr, tgt.old_values, tgt.count * sizeof(duckvalue_t *));
            }
            if (tgt.new_values) cloudsync_memory_free(tgt.new_values);
            if (tgt.old_values) cloudsync_memory_free(tgt.old_values);
            tgt.new_values = new_arr;
            tgt.old_values = old_arr;
            tgt.capacity = newcap;
        }

        // Move source entries into target
        memcpy(tgt.new_values + tgt.count, src.new_values, src.count * sizeof(duckvalue_t *));
        memcpy(tgt.old_values + tgt.count, src.old_values, src.count * sizeof(duckvalue_t *));
        tgt.count = new_count;

        // Source entries are now owned by target — clear source without freeing values
        if (src.new_values) cloudsync_memory_free(src.new_values);
        if (src.old_values) cloudsync_memory_free(src.old_values);
        if (src.table_name) duckvalue_free(src.table_name);
        src.new_values = nullptr;
        src.old_values = nullptr;
        src.table_name = nullptr;
        src.count = 0;
        src.capacity = 0;
    }
}

static void UpdateAggFreeState(UpdateAggState &state) {
    for (int i = 0; i < state.count; i++) {
        if (state.new_values[i]) duckvalue_free(state.new_values[i]);
        if (state.old_values[i]) duckvalue_free(state.old_values[i]);
    }
    if (state.new_values) cloudsync_memory_free(state.new_values);
    if (state.old_values) cloudsync_memory_free(state.old_values);
    if (state.table_name) duckvalue_free(state.table_name);
    state.new_values = nullptr;
    state.old_values = nullptr;
    state.table_name = nullptr;
    state.count = 0;
    state.capacity = 0;
}

static void UpdateAggFinalize(Vector &states, AggregateInputData &aggr_input, Vector &result, idx_t count, idx_t offset) {
    auto state_ptrs = FlatVector::GetData<UpdateAggState *>(states);

    cloudsync_context *data = aggr_input.bind_data ? aggr_input.bind_data->Cast<CloudSyncBindData>().db_state->context : nullptr;
    if (!data) throw InternalException("CloudSync not initialized");

    for (idx_t i = 0; i < count; i++) {
        auto &state = *state_ptrs[i];
        idx_t ridx = i + offset;

        if (!state.table_name || state.count == 0) {
            result.SetValue(ridx, Value::BOOLEAN(true));
            continue;
        }

        const char *table_name = database_value_text((dbvalue_t *)state.table_name);
        cloudsync_table_context *table = table_lookup(data, table_name);
        if (!table) {
            char meta_name[1024];
            snprintf(meta_name, sizeof(meta_name), "%s_cloudsync", table_name);
            if (!database_table_exists(data, meta_name, cloudsync_schema(data))) {
                UpdateAggFreeState(state);
                throw InvalidInputException("Unable to retrieve table name %s in cloudsync_update", table_name);
            }
            table_algo algo = dbutils_table_settings_get_algo(data, table_name);
            if (algo == table_algo_none) algo = table_algo_crdt_cls;
            if (!table_add_to_context(data, algo, table_name)) {
                UpdateAggFreeState(state);
                throw InternalException("Unable to load table context for %s", table_name);
            }
            table = table_lookup(data, table_name);
            if (!table) {
                UpdateAggFreeState(state);
                throw InvalidInputException("Unable to retrieve table name %s in cloudsync_update", table_name);
            }
        }

        // Check filter — skip tracking if row doesn't match
        {
            char fbuf[2048];
            int frc = dbutils_table_settings_get_value(data, table_name, "*", "filter", fbuf, sizeof(fbuf));
            if (frc == DBRES_OK && fbuf[0] != 0) {
                int npks = table_count_pks(table);
                // Build query: SELECT 1 FROM table WHERE (filter) AND pk1='v1' ...
                string sql = "SELECT 1 FROM \"" + string(table_name) + "\" WHERE (" + string(fbuf) + ")";
                char **pknames = table_pknames(table);
                if (!pknames) {
                    char **arr = NULL; int ncount = 0;
                    if (database_pk_names(data, table_name, &arr, &ncount) == DBRES_OK && arr) {
                        table_set_pknames(table, arr);
                        pknames = arr;
                    }
                }
                if (pknames && state.count >= npks) {
                    for (int p = 0; p < npks; p++) {
                        const char *v = database_value_text((dbvalue_t *)state.new_values[p]);
                        if (!v) {
                            sql += " AND \"" + string(pknames[p]) + "\" IS NULL";
                        } else {
                            string vs(v);
                            string esc;
                            for (auto c : vs) { if (c == '\'') esc += "''"; else esc += c; }
                            sql += " AND \"" + string(pknames[p]) + "\" = '" + esc + "'";
                        }
                    }
                    sql += " LIMIT 1";
                    Connection *conn = (Connection *)cloudsync_db(data);
                    if (conn) {
                        try {
                            auto qr = conn->Query(sql);
                            if (!qr->HasError()) {
                                auto chunk = qr->Fetch();
                                if (!chunk || chunk->size() == 0) {
                                    // Row doesn't match filter — skip
                                    UpdateAggFreeState(state);
                                    result.SetValue(ridx, Value::BOOLEAN(true));
                                    continue;
                                }
                            }
                        } catch (...) {}
                    }
                }
            }
        }

        int64_t db_version = cloudsync_dbversion_next(data, CLOUDSYNC_VALUE_NOTSET);
        int pk_count = table_count_pks(table);

        if (state.count < pk_count) {
            UpdateAggFreeState(state);
            throw InvalidInputException("Not enough primary key values in cloudsync_update payload");
        }
        int max_expected = pk_count + table_count_cols(table);
        if (state.count > max_expected) {
            UpdateAggFreeState(state);
            throw InvalidInputException("Too many values in cloudsync_update payload: got %d expected <= %d", state.count, max_expected);
        }

        // Check if primary key changed
        bool prikey_changed = false;
        for (int j = 0; j < pk_count; j++) {
            if (dbutils_value_compare((dbvalue_t *)state.old_values[j], (dbvalue_t *)state.new_values[j]) != 0) {
                prikey_changed = true;
                break;
            }
        }

        // Encode primary key
        char pk_buffer[1024];
        size_t pklen = sizeof(pk_buffer);
        char *pk = pk_encode_prikey((dbvalue_t **)state.new_values, pk_count, pk_buffer, &pklen);
        if (!pk) {
            UpdateAggFreeState(state);
            throw InternalException("Not enough memory to encode the primary key(s)");
        }

        int rc = DBRES_OK;

        if (prikey_changed) {
            char pk_buffer2[1024];
            size_t oldpklen = sizeof(pk_buffer2);
            char *oldpk = pk_encode_prikey((dbvalue_t **)state.old_values, pk_count, pk_buffer2, &oldpklen);
            if (!oldpk) {
                if (pk != pk_buffer) cloudsync_memory_free(pk);
                UpdateAggFreeState(state);
                throw InternalException("Not enough memory to encode the old primary key(s)");
            }

            rc = local_mark_delete_meta(table, oldpk, oldpklen, db_version, cloudsync_bumpseq(data));
            if (rc == DBRES_OK) rc = local_update_move_meta(table, pk, pklen, oldpk, oldpklen, db_version);
            if (rc == DBRES_OK) rc = local_mark_insert_sentinel_meta(table, pk, pklen, db_version, cloudsync_bumpseq(data));

            if (oldpk != pk_buffer2) cloudsync_memory_free(oldpk);
        }

        // Compare each non-PK column
        if (rc == DBRES_OK) {
            for (int j = 0; j < table_count_cols(table); j++) {
                int col_index = pk_count + j;
                if (col_index >= state.count) break;

                if (dbutils_value_compare((dbvalue_t *)state.old_values[col_index], (dbvalue_t *)state.new_values[col_index]) != 0) {
                    rc = local_mark_insert_or_update_meta(table, pk, pklen, table_colname(table, j), db_version, cloudsync_bumpseq(data));
                    if (rc != DBRES_OK) break;
                }
            }
        }

        if (pk != pk_buffer) cloudsync_memory_free(pk);
        UpdateAggFreeState(state);

        if (rc != DBRES_OK) {
            throw InternalException("%s", database_errmsg(data));
        }

        result.SetValue(ridx, Value::BOOLEAN(true));
    }
}

static void UpdateAggDestructor(Vector &states, AggregateInputData &, idx_t count) {
    auto state_ptrs = FlatVector::GetData<UpdateAggState *>(states);
    for (idx_t i = 0; i < count; i++) {
        UpdateAggFreeState(*state_ptrs[i]);
    }
}

// MARK: - Extension Loading

static CloudSyncDatabaseState *InitCloudSyncContext(DatabaseInstance &db) {
    auto db_state = make_uniq<CloudSyncDatabaseState>();
    db_state->db_instance = &db;
    db_state->connection = make_uniq<Connection>(db);

    db_state->context = cloudsync_context_create((void *)db_state->connection.get());
    if (!db_state->context) {
        throw InternalException("Failed to create CloudSync context");
    }

    // Store state in global map BEFORE calling cloudsync_context_init, because
    // context_init loads settings which issues queries that trigger bind callbacks
    // which call GetOrCreateDatabaseState — without early registration this
    // causes infinite recursion.
    auto *result = db_state.get();
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_states[&db] = std::move(db_state);
    }

    // Initialize if config already exists (persistent DB reopen)
    if (cloudsync_config_exists(result->context)) {
        if (cloudsync_context_init(result->context) == NULL) {
            // Not fatal — settings table may not exist yet
        }
        dbutils_settings_set_key_value(result->context, CLOUDSYNC_KEY_LIBVERSION, CLOUDSYNC_VERSION);
    }

    return result;
}

// Helpers to create ScalarFunctions with proper stability/side_effects.
// Pure: deterministic, no side effects (default DuckDB behavior).
// Volatile: may return different results, no side effects.
// SideEffect: volatile + writes state (prevents optimizer elimination/reordering).

static ScalarFunction VolatileFunction(string name, vector<LogicalType> args, LogicalType ret, scalar_function_t fun) {
    ScalarFunction func(std::move(name), std::move(args), std::move(ret), std::move(fun));
    func.stability = FunctionStability::VOLATILE;
    func.bind = CloudSyncScalarBind;
    return func;
}

static ScalarFunction SideEffectFunction(string name, vector<LogicalType> args, LogicalType ret, scalar_function_t fun) {
    ScalarFunction func(std::move(name), std::move(args), std::move(ret), std::move(fun));
    func.stability = FunctionStability::VOLATILE;
    func.bind = CloudSyncScalarBind;
    return func;
}

static ScalarFunction SideEffectFunctionNoName(vector<LogicalType> args, LogicalType ret, scalar_function_t fun) {
    ScalarFunction func(std::move(args), std::move(ret), std::move(fun));
    func.stability = FunctionStability::VOLATILE;
    func.bind = CloudSyncScalarBind;
    return func;
}

static void LoadInternal(ExtensionLoader &loader) {
    auto &db = loader.GetDatabaseInstance();

    // Initialize memory system
    cloudsync_memory_init(1);

    // NOTE: CloudSync context is initialized AFTER all function registrations below,
    // because re-opening an existing database triggers settings loading which
    // prepares SQL referencing CloudSync functions (e.g. cloudsync_db_version_next).

    // --- Pure functions (deterministic, no side effects) ---

    // cloudsync_version() → VARCHAR
    loader.RegisterFunction(ScalarFunction("cloudsync_version", {}, LogicalType::VARCHAR, CloudSyncVersionFun));

    // cloudsync_pk_encode(...) → BLOB (1-5 argument variants)
    {
        ScalarFunctionSet set("cloudsync_pk_encode");
        set.AddFunction(ScalarFunction({LogicalType::ANY}, LogicalType::BLOB, CloudSyncPkEncodeFun));
        set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::BLOB, CloudSyncPkEncodeFun));
        set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BLOB, CloudSyncPkEncodeFun));
        set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BLOB, CloudSyncPkEncodeFun));
        set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BLOB, CloudSyncPkEncodeFun));
        loader.RegisterFunction(set);
    }

    // cloudsync_pk_decode(pk, index) → VARCHAR
    loader.RegisterFunction(ScalarFunction("cloudsync_pk_decode", {LogicalType::BLOB, LogicalType::INTEGER}, LogicalType::VARCHAR, CloudSyncPkDecodeFun));

    // cloudsync_value_encode(value) → BLOB (single value, no element count prefix)
    // Must use SPECIAL_HANDLING so NULL inputs are encoded (not skipped)
    {
        ScalarFunction val_enc("cloudsync_value_encode", {LogicalType::ANY}, LogicalType::BLOB, CloudSyncValueEncodeFun);
        val_enc.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
        loader.RegisterFunction(val_enc);
    }

    // --- Volatile functions (read state, may return different results) ---

    // cloudsync_txn_id() → BIGINT (transaction ID from catalog, used by SQL_DATA_VERSION)
    loader.RegisterFunction(VolatileFunction("cloudsync_txn_id", {}, LogicalType::BIGINT, CloudSyncTxnIdFun));

    // cloudsync_uuid() → VARCHAR
    loader.RegisterFunction(VolatileFunction("cloudsync_uuid", {}, LogicalType::VARCHAR, CloudSyncUuidFun));

    // cloudsync_siteid() → BLOB
    loader.RegisterFunction(VolatileFunction("cloudsync_siteid", {}, LogicalType::BLOB, CloudSyncSiteidFun));

    // cloudsync_db_version() → BIGINT
    loader.RegisterFunction(VolatileFunction("cloudsync_db_version", {}, LogicalType::BIGINT, CloudSyncDbVersionFun));

    // cloudsync_schema() → VARCHAR
    loader.RegisterFunction(VolatileFunction("cloudsync_schema", {}, LogicalType::VARCHAR, CloudSyncSchemaFun));

    // cloudsync_table_schema(table) → VARCHAR
    loader.RegisterFunction(VolatileFunction("cloudsync_table_schema", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CloudSyncTableSchemaFun));

    // cloudsync_is_enabled(table) → BOOLEAN
    loader.RegisterFunction(VolatileFunction("cloudsync_is_enabled", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncIsEnabledFun));

    // cloudsync_is_sync(table) → BOOLEAN
    loader.RegisterFunction(VolatileFunction("cloudsync_is_sync", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncIsSyncFun));

    // cloudsync_col_value(table, col, pk) → VARCHAR (raw column value, like SQLite)
    loader.RegisterFunction(VolatileFunction("cloudsync_col_value", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BLOB}, LogicalType::VARCHAR, CloudSyncColValueFun));

    // --- Side-effect functions (write state, must not be eliminated/reordered) ---

    // cloudsync_db_version_next() → BIGINT (0 or 1 arg)
    {
        ScalarFunctionSet set("cloudsync_db_version_next");
        set.AddFunction(SideEffectFunctionNoName({}, LogicalType::BIGINT, CloudSyncDbVersionNextFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::BIGINT}, LogicalType::BIGINT, CloudSyncDbVersionNextFun));
        loader.RegisterFunction(set);
    }

    // cloudsync_init(table, [algo], [skip_int_pk_check]) → BLOB
    {
        ScalarFunctionSet set("cloudsync_init");
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR}, LogicalType::BLOB, CloudSyncInitFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BLOB, CloudSyncInitFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::BLOB, CloudSyncInitFun));
        loader.RegisterFunction(set);
    }

    // cloudsync_enable(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_enable", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncEnableFun));

    // cloudsync_disable(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_disable", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncDisableFun));

    // cloudsync_cleanup(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_cleanup", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncCleanupFun));

    // cloudsync_terminate() → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_terminate", {}, LogicalType::BOOLEAN, CloudSyncTerminateFun));

    // cloudsync_set(key, value) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_set", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncSetFun));

    // cloudsync_set_table(table, key, value) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_set_table", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncSetTableFun));

    // cloudsync_set_column(table, col, key, value) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_set_column", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncSetColumnFun));

    // cloudsync_set_filter(table, filter) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_set_filter", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncSetFilterFun));

    // cloudsync_clear_filter(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_clear_filter", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncClearFilterFun));

    // cloudsync_set_schema(schema) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_set_schema", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncSetSchemaFun));

    // cloudsync_begin_alter(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_begin_alter", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncBeginAlterFun));

    // cloudsync_commit_alter(table) → BOOLEAN
    loader.RegisterFunction(SideEffectFunction("cloudsync_commit_alter", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CloudSyncCommitAlterFun));

    // cloudsync_seq() → INTEGER
    loader.RegisterFunction(SideEffectFunction("cloudsync_seq", {}, LogicalType::INTEGER, CloudSyncSeqFun));

    // cloudsync_insert(table, pk_values...) → BOOLEAN (2-6 argument variants)
    {
        ScalarFunctionSet set("cloudsync_insert");
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncInsertFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncInsertFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncInsertFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncInsertFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncInsertFun));
        loader.RegisterFunction(set);
    }

    // cloudsync_delete(table, pk_values...) → BOOLEAN (2-6 argument variants)
    {
        ScalarFunctionSet set("cloudsync_delete");
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncDeleteFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncDeleteFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncDeleteFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncDeleteFun));
        set.AddFunction(SideEffectFunctionNoName({LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY, LogicalType::ANY}, LogicalType::BOOLEAN, CloudSyncDeleteFun));
        loader.RegisterFunction(set);
    }

    // cloudsync_payload_apply(payload) → INTEGER (alias: cloudsync_payload_decode)
    loader.RegisterFunction(SideEffectFunction("cloudsync_payload_apply", {LogicalType::BLOB}, LogicalType::INTEGER, CloudSyncPayloadApplyFun));
    loader.RegisterFunction(SideEffectFunction("cloudsync_payload_decode", {LogicalType::BLOB}, LogicalType::INTEGER, CloudSyncPayloadApplyFun));

    // cloudsync_payload_save(path) → BIGINT
    loader.RegisterFunction(SideEffectFunction("cloudsync_payload_save", {LogicalType::VARCHAR}, LogicalType::BIGINT, CloudSyncPayloadSaveFun));

    // cloudsync_payload_load(path) → INTEGER
    loader.RegisterFunction(SideEffectFunction("cloudsync_payload_load", {LogicalType::VARCHAR}, LogicalType::INTEGER, CloudSyncPayloadLoadFun));

    // cloudsync_merge_insert(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) → BIGINT
    // Internal: used by cloudsync_payload_apply to merge incoming changes
    loader.RegisterFunction(SideEffectFunction("cloudsync_merge_insert",
        {LogicalType::VARCHAR, LogicalType::BLOB, LogicalType::VARCHAR, LogicalType::ANY,
         LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BLOB, LogicalType::BIGINT, LogicalType::BIGINT},
        LogicalType::BIGINT, CloudSyncMergeInsertFun));

    // --- Aggregate functions ---

    // cloudsync_payload_encode(text, bytea, text, bytea, bigint, bigint, bytea, bigint, bigint) → BLOB
    {
        AggregateFunction payload_encode(
            "cloudsync_payload_encode",
            {LogicalType::VARCHAR, LogicalType::BLOB, LogicalType::VARCHAR, LogicalType::BLOB,
             LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BLOB, LogicalType::BIGINT, LogicalType::BIGINT},
            LogicalType::BLOB,
            AggregateFunction::StateSize<PayloadEncodeState>,
            PayloadEncodeInit,
            PayloadEncodeUpdate,
            PayloadEncodeCombine,
            PayloadEncodeFinalize,
            FunctionNullHandling::DEFAULT_NULL_HANDLING,
            nullptr, CloudSyncAggregateBind,
            PayloadEncodeDestructor
        );
        payload_encode.stability = FunctionStability::VOLATILE;
        payload_encode.order_dependent = AggregateOrderDependent::ORDER_DEPENDENT;
        loader.RegisterFunction(payload_encode);
    }

    // cloudsync_update(text, any, any) → BOOLEAN
    {
        AggregateFunction update_agg(
            "cloudsync_update",
            {LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY},
            LogicalType::BOOLEAN,
            AggregateFunction::StateSize<UpdateAggState>,
            UpdateAggInit,
            UpdateAggUpdate,
            UpdateAggCombine,
            UpdateAggFinalize,
            FunctionNullHandling::SPECIAL_HANDLING,
            nullptr, CloudSyncAggregateBind,
            UpdateAggDestructor
        );
        update_agg.stability = FunctionStability::VOLATILE;
        loader.RegisterFunction(update_agg);
    }

    // --- Table functions ---

    // cloudsync_changes_select(min_db_version BIGINT, filter_site_id BLOB) → TABLE
    {
        TableFunctionSet set("cloudsync_changes_select");
        set.AddFunction(TableFunction({}, ChangesSelectFunction, ChangesSelectBind, ChangesSelectInitGlobal));
        set.AddFunction(TableFunction({LogicalType::BIGINT}, ChangesSelectFunction, ChangesSelectBind, ChangesSelectInitGlobal));
        set.AddFunction(TableFunction({LogicalType::BIGINT, LogicalType::BLOB}, ChangesSelectFunction, ChangesSelectBind, ChangesSelectInitGlobal));
        loader.RegisterFunction(set);
    }

    // CloudSync context is lazily initialized on first function call via
    // GetOrCreateDatabaseState(). This avoids issues with extension load order
    // (core_functions must be available for SQL like string_agg).

    // cloudsync_changes view
    {
        Connection view_conn(db);
        view_conn.Query("CREATE OR REPLACE VIEW cloudsync_changes AS "
                       "SELECT * FROM cloudsync_changes_select(0::BIGINT, NULL::BLOB)");
    }
}

// MARK: - Extension class

namespace duckdb {

void CloudsyncExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string CloudsyncExtension::Name() {
    return "cloudsync";
}

std::string CloudsyncExtension::Version() const {
    return CLOUDSYNC_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cloudsync, loader) {
    LoadInternal(loader);
}

}
