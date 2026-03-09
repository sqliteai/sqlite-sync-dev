//
//  sql_duckdb.c
//  cloudsync
//
//  DuckDB-specific SQL queries
//
//  DuckDB SQL dialect notes:
//  - Parameters use $1, $2, ... positional syntax
//  - Has PRAGMA table_info() like SQLite
//  - Has information_schema like PostgreSQL
//  - ON CONFLICT ... DO UPDATE SET supported
//  - RETURNING clause supported
//  - No triggers (change tracking must be manual)
//  - No rowid (all operations are PK-based)
//  - No COLLATE NOCASE (use lower() for case-insensitive)
//  - No REPLACE INTO (use ON CONFLICT)
//  - No WITHOUT ROWID (no rowid concept)
//  - format() uses {} not %I/%s
//

#include "../sql.h"

// MARK: Settings

const char * const SQL_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_settings WHERE key=$1;";

const char * const SQL_SETTINGS_SET_KEY_VALUE_REPLACE =
    "INSERT INTO cloudsync_settings (key, value) VALUES ($1, $2) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;";

const char * const SQL_SETTINGS_SET_KEY_VALUE_DELETE =
    "DELETE FROM cloudsync_settings WHERE key = $1;";

const char * const SQL_TABLE_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_table_settings WHERE (tbl_name=$1 AND col_name=$2 AND key=$3);";

const char * const SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE =
    "DELETE FROM cloudsync_table_settings WHERE tbl_name=$1;";

const char * const SQL_TABLE_SETTINGS_REPLACE =
    "INSERT INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES ($1, $2, $3, $4) "
    "ON CONFLICT (tbl_name, key) DO UPDATE SET col_name = EXCLUDED.col_name, value = EXCLUDED.value;";

const char * const SQL_TABLE_SETTINGS_DELETE_ONE =
    "DELETE FROM cloudsync_table_settings WHERE (tbl_name=$1 AND col_name=$2 AND key=$3);";

const char * const SQL_TABLE_SETTINGS_COUNT_TABLES =
    "SELECT count(*) FROM cloudsync_table_settings WHERE key='algo';";

const char * const SQL_SETTINGS_LOAD_GLOBAL =
    "SELECT key, value FROM cloudsync_settings;";

const char * const SQL_SETTINGS_LOAD_TABLE =
    "SELECT lower(tbl_name), lower(col_name), key, value FROM cloudsync_table_settings ORDER BY tbl_name;";

const char * const SQL_CREATE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_settings (key VARCHAR PRIMARY KEY NOT NULL, value VARCHAR);";

// format strings (snprintf) are also static SQL templates
const char * const SQL_INSERT_SETTINGS_STR_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', '%s');";

const char * const SQL_INSERT_SETTINGS_INT_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', %lld);";

const char * const SQL_CREATE_SITE_ID_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_site_id ("
    "id BIGINT PRIMARY KEY, "
    "site_id BLOB UNIQUE NOT NULL"
    ");";

const char * const SQL_INSERT_SITE_ID_ROWID =
    "INSERT INTO cloudsync_site_id (id, site_id) VALUES ($1, $2);";

const char * const SQL_CREATE_TABLE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_table_settings (tbl_name VARCHAR NOT NULL, col_name VARCHAR NOT NULL, key VARCHAR, value VARCHAR, PRIMARY KEY(tbl_name,key));";

const char * const SQL_CREATE_SCHEMA_VERSIONS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (hash BIGINT PRIMARY KEY, seq INTEGER NOT NULL)";

const char * const SQL_SETTINGS_CLEANUP_DROP_ALL =
    "DROP TABLE IF EXISTS cloudsync_settings; "
    "DROP TABLE IF EXISTS cloudsync_site_id; "
    "DROP TABLE IF EXISTS cloudsync_table_settings; "
    "DROP TABLE IF EXISTS cloudsync_schema_versions; ";

// MARK: CloudSync

// DuckDB: Build a UNION ALL query across all _cloudsync meta tables to find max db_version.
// Uses information_schema.tables since DuckDB has no pg_tables.
const char * const SQL_DBVERSION_BUILD_QUERY =
    "WITH table_names AS ("
    "SELECT '\"' || table_name || '\"' as tbl_name "
    "FROM information_schema.tables "
    "WHERE table_schema='main' "
    "AND table_name LIKE '%_cloudsync'"
    "), "
    "query_parts AS ("
    "SELECT 'SELECT COALESCE(MAX(db_version), 0) as version FROM ' || tbl_name as part "
    "FROM table_names"
    "), "
    "combined_query AS ("
    "SELECT string_agg(part, ' UNION ALL ') "
    "|| ' UNION SELECT CAST(value AS BIGINT) as version FROM cloudsync_settings WHERE key = ''pre_alter_dbversion''' "
    "as full_query FROM query_parts"
    ") "
    "SELECT 'SELECT COALESCE(MAX(version), 0) as version FROM (' || full_query || ');' FROM combined_query;";

const char * const SQL_SITEID_SELECT_ROWID0 =
    "SELECT site_id FROM cloudsync_site_id WHERE id=0;";

// DuckDB has no data_version PRAGMA equivalent.
// Return an always-incrementing value so dbvm_execute always reports CHANGED,
// forcing db_version recomputation from meta tables on every access.
// This is safe because the recomputation is cheap (single MAX query).
// DuckDB has no PRAGMA data_version. Use a constant so dbvm_execute always
// Similar to PostgreSQL's txid_snapshot_xmin(txid_current_snapshot()),
// reads the global transaction ID from the DuckDB catalog to detect
// when another connection has committed changes.
const char * const SQL_DATA_VERSION =
    "SELECT cloudsync_txn_id();";

// DuckDB has no schema_version PRAGMA; track via settings
const char * const SQL_SCHEMA_VERSION =
    "SELECT COALESCE((SELECT CAST(value AS BIGINT) FROM cloudsync_settings WHERE key='schemaversion'), 0);";

const char * const SQL_SITEID_GETSET_ROWID_BY_SITEID =
    "INSERT INTO cloudsync_site_id (id, site_id) VALUES ("
    "COALESCE((SELECT MAX(id) FROM cloudsync_site_id), 0) + 1, $1) "
    "ON CONFLICT(site_id) DO UPDATE SET site_id = EXCLUDED.site_id "
    "RETURNING id;";

// MARK: SQL builders (format strings for cloudsync_memory_mprintf)

// DuckDB has PRAGMA table_info like SQLite; these use it directly.
const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID =
    "WITH col_names AS ("
    "SELECT string_agg('\"' || name || '\"', ',' ORDER BY cid) AS cols "
    "FROM pragma_table_info('%s') WHERE pk=0"
    ") "
    "SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM \"%s\" LIMIT 0;'";

const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_PK =
    "WITH col_names AS ("
    "SELECT string_agg('\"' || name || '\"', ',' ORDER BY cid) AS cols "
    "FROM pragma_table_info('%s') WHERE pk=0"
    "), "
    "pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    "), "
    "pk_where AS ("
    "SELECT string_agg('\"' || name || '\"=$' || CAST(pk_idx AS VARCHAR), ' AND ' ORDER BY pk_idx) AS pk_clause "
    "FROM pk_numbered"
    ") "
    "SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM \"%s\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_DELETE_ROW_BY_ROWID =
    "DELETE FROM \"%s\" WHERE false;";  // DuckDB has no rowid; this is a no-op placeholder

const char * const SQL_BUILD_DELETE_ROW_BY_PK =
    "WITH pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    "), "
    "pk_where AS ("
    "SELECT string_agg('\"' || name || '\"=$' || CAST(pk_idx AS VARCHAR), ' AND ' ORDER BY pk_idx) AS pk_clause "
    "FROM pk_numbered"
    ") "
    "SELECT 'DELETE FROM \"%s\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_INSERT_ROWID_IGNORE =
    "SELECT 0;";  // DuckDB has no rowid; no-op placeholder

const char * const SQL_UPSERT_ROWID_AND_COL_BY_ROWID =
    "SELECT 0;";  // DuckDB has no rowid; no-op placeholder

const char * const SQL_BUILD_INSERT_PK_IGNORE =
    "WITH pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    "), "
    "pk_cols AS ("
    "SELECT string_agg('\"' || name || '\"', ',' ORDER BY pk_idx) AS pk_clause "
    "FROM pk_numbered"
    "), "
    "pk_bind AS ("
    "SELECT string_agg('$' || CAST(pk_idx AS VARCHAR), ',' ORDER BY pk_idx) AS pk_binding "
    "FROM pk_numbered"
    ") "
    "SELECT 'INSERT INTO \"%s\" (' || (SELECT pk_clause FROM pk_cols) || ') VALUES (' "
    "|| (SELECT pk_binding FROM pk_bind) || ') ON CONFLICT DO NOTHING;'";

const char * const SQL_BUILD_UPSERT_PK_AND_COL =
    "WITH pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    "), "
    "pk_cols AS ("
    "SELECT string_agg('\"' || name || '\"', ',' ORDER BY pk_idx) AS pk_clause "
    "FROM pk_numbered"
    "), "
    "pk_bind AS ("
    "SELECT string_agg('$' || CAST(pk_idx AS VARCHAR), ',' ORDER BY pk_idx) AS pk_binding "
    "FROM pk_numbered"
    "), "
    "pk_count AS ("
    "SELECT count(*) AS n FROM pk_numbered"
    ") "
    "SELECT 'INSERT INTO \"%s\" (' || (SELECT pk_clause FROM pk_cols) || ',\"%s\") VALUES (' "
    "|| (SELECT pk_binding FROM pk_bind) || ',$' || CAST((SELECT n FROM pk_count) + 1 AS VARCHAR) "
    "|| ') ON CONFLICT (' || (SELECT pk_clause FROM pk_cols) || ') DO UPDATE SET \"%s\"=$' "
    "|| CAST((SELECT n FROM pk_count) + 2 AS VARCHAR) || ';'";

const char * const SQL_SELECT_COLS_BY_ROWID_FMT =
    "SELECT 0;";  // DuckDB has no rowid; no-op placeholder

const char * const SQL_BUILD_SELECT_COLS_BY_PK_FMT =
    "WITH pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    "), "
    "pk_where AS ("
    "SELECT string_agg('\"' || name || '\"=$' || CAST(pk_idx AS VARCHAR), ' AND ' ORDER BY pk_idx) AS pk_clause "
    "FROM pk_numbered"
    ") "
    "SELECT 'SELECT \"%s\" FROM \"%s\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_CLOUDSYNC_ROW_EXISTS_BY_PK =
    "SELECT EXISTS(SELECT 1 FROM %s WHERE pk = $1 LIMIT 1);";

const char * const SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION =
    "UPDATE %s "
    "SET col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, "
    "db_version = $1, seq = $2, site_id = 0 "
    "WHERE pk = $3 AND col_name = '%s';";

const char * const SQL_CLOUDSYNC_UPSERT_COL_INIT_OR_BUMP_VERSION =
    "INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, '%s', 1, $2, $3, 0) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = CASE %s.col_version %% 2 WHEN 0 THEN %s.col_version + 1 ELSE %s.col_version + 2 END, "
    "db_version = $4, seq = $5, site_id = 0;";

const char * const SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION =
    "INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, $2, $3, $4, $5, 0) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = %s.col_version + 1, db_version = $6, seq = $7, site_id = 0;";

const char * const SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL =
    "DELETE FROM %s WHERE pk = $1 AND col_name != '%s';";

// DuckDB does not support writable CTEs, so use INSERT ... SELECT directly.
// The DELETE of old pk rows is handled separately by meta_merge_delete_drop.
const char * const SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL =
    "INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "SELECT $1, col_name, 1, $2, cloudsync_seq(), 0 "
    "FROM %s WHERE pk = $3 AND col_name != '%s' "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = 1, db_version = $2, seq = cloudsync_seq(), site_id = 0;";

const char * const SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS =
    "SELECT COALESCE("
    "(SELECT col_version FROM %s WHERE pk = $1 AND col_name = '%s'), "
    "(SELECT 1 FROM %s WHERE pk = $2 LIMIT 1)"
    ");";

const char * const SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID =
    "INSERT INTO %s "
    "(pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, $2, $3, cloudsync_db_version_next($4), $5, $6) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = EXCLUDED.col_version, "
    "db_version = cloudsync_db_version_next($4), "
    "seq = EXCLUDED.seq, "
    "site_id = EXCLUDED.site_id "
    "RETURNING (CAST(db_version AS BIGINT) * 1073741824 + seq);";

const char * const SQL_CLOUDSYNC_TOMBSTONE_PK_EXCEPT_COL =
    "UPDATE %s "
    "SET col_version = 0, db_version = cloudsync_db_version_next($1) "
    "WHERE pk = $2 AND col_name != '%s';";

const char * const SQL_CLOUDSYNC_SELECT_COL_VERSION_BY_PK_COL =
    "SELECT col_version FROM %s WHERE pk = $1 AND col_name = $2;";

const char * const SQL_CLOUDSYNC_SELECT_SITE_ID_BY_PK_COL =
    "SELECT site_id FROM %s WHERE pk = $1 AND col_name = $2;";

// DuckDB has PRAGMA table_info like SQLite
const char * const SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID =
    "SELECT name, cid FROM pragma_table_info('%s') WHERE pk=0 ORDER BY cid;";

const char * const SQL_DROP_CLOUDSYNC_TABLE =
    "DROP TABLE IF EXISTS %s;";

const char * const SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL =
    "DELETE FROM %s WHERE col_name NOT IN ("
    "SELECT name FROM pragma_table_info('%s') UNION SELECT '%s'"
    ");";

const char * const SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT =
    "SELECT string_agg('\"%s\".\"' || name || '\"', ',' ORDER BY cid) "
    "FROM pragma_table_info('%s') WHERE pk>0;";

const char * const SQL_CLOUDSYNC_GC_DELETE_ORPHANED_PK =
    "DELETE FROM %s "
    "WHERE (col_name != '%s' OR (col_name = '%s' AND col_version %% 2 != 0)) "
    "AND NOT EXISTS ("
    "SELECT 1 FROM %s "
    "WHERE %s.pk = cloudsync_pk_encode(%s) LIMIT 1"
    ");";

const char * const SQL_PRAGMA_TABLEINFO_PK_COLLIST =
    "SELECT string_agg('\"' || name || '\"', ',' ORDER BY cid) "
    "FROM pragma_table_info('%s') WHERE pk>0;";

const char * const SQL_PRAGMA_TABLEINFO_PK_DECODE_SELECTLIST =
    "WITH pk_numbered AS ("
    "SELECT name, ROW_NUMBER() OVER (ORDER BY cid) AS pk_idx "
    "FROM pragma_table_info('%s') WHERE pk>0"
    ") "
    "SELECT string_agg("
    "'cloudsync_pk_decode(pk, ' || CAST(pk_idx AS VARCHAR) || ') AS \"' || name || '\"', ',' ORDER BY pk_idx"
    ") "
    "FROM pk_numbered;";

const char * const SQL_CLOUDSYNC_INSERT_MISSING_PKS_FROM_BASE_EXCEPT_SYNC =
    "SELECT cloudsync_insert('%s', %s) "
    "FROM (SELECT %s FROM %s EXCEPT SELECT %s FROM %s);";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM %s) "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM %s _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = $1"
    ");";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL_FILTERED =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM %s WHERE (%s)) "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM %s _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = $1"
    ");";

const char * const SQL_CHANGES_INSERT_ROW =
    "SELECT cloudsync_merge_insert($1,$2,$3,$4,$5,$6,$7,$8,$9);";
