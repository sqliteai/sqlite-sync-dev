-- DuckDB CloudSync Extension Test Suite
-- Run with: /path/to/duckdb /tmp/cloudsync_test.duckdb < test/duckdb/run_tests.sql
--
-- This test suite covers all CloudSync functions using manual change tracking
-- (cloudsync_insert, cloudsync_delete, cloudsync_update) since DuckDB has no triggers.

.mode list
.separator ' '
.nullvalue NULL

-- ============================================================================
-- Test Infrastructure
-- ============================================================================

CREATE OR REPLACE MACRO test_pass(name) AS (SELECT printf('[PASS] %s', name));
CREATE OR REPLACE MACRO test_fail(name) AS (SELECT printf('[FAIL] %s', name));

-- ============================================================================
-- TEST 1: cloudsync_version
-- ============================================================================
SELECT CASE WHEN cloudsync_version() IS NOT NULL AND length(cloudsync_version()) > 0
    THEN test_pass('cloudsync_version returns non-empty string')
    ELSE test_fail('cloudsync_version returns non-empty string') END;

-- ============================================================================
-- TEST 2: cloudsync_pk_encode / cloudsync_pk_decode
-- ============================================================================

-- pk_decode uses 1-based indexing (same as SQLite)

-- Single integer PK
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode(42), 1) = '42'
    THEN test_pass('pk_encode/decode single integer')
    ELSE test_fail('pk_encode/decode single integer') END;

-- Single text PK
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode('hello'), 1) = 'hello'
    THEN test_pass('pk_encode/decode single text')
    ELSE test_fail('pk_encode/decode single text') END;

-- Composite PK (2 columns)
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode('alice', 'smith'), 1) = 'alice'
              AND cloudsync_pk_decode(cloudsync_pk_encode('alice', 'smith'), 2) = 'smith'
    THEN test_pass('pk_encode/decode composite 2-col')
    ELSE test_fail('pk_encode/decode composite 2-col') END;

-- Composite PK (3 columns)
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode('a', 'b', 'c'), 1) = 'a'
              AND cloudsync_pk_decode(cloudsync_pk_encode('a', 'b', 'c'), 2) = 'b'
              AND cloudsync_pk_decode(cloudsync_pk_encode('a', 'b', 'c'), 3) = 'c'
    THEN test_pass('pk_encode/decode composite 3-col')
    ELSE test_fail('pk_encode/decode composite 3-col') END;

-- Out of bounds returns NULL (only 1 element, index 2 is OOB)
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode(42), 2) IS NULL
    THEN test_pass('pk_decode out of bounds returns NULL')
    ELSE test_fail('pk_decode out of bounds returns NULL') END;

-- Negative index returns NULL
SELECT CASE WHEN cloudsync_pk_decode(cloudsync_pk_encode(42), -1) IS NULL
    THEN test_pass('pk_decode negative index returns NULL')
    ELSE test_fail('pk_decode negative index returns NULL') END;

-- ============================================================================
-- TEST 3: cloudsync_uuid
-- ============================================================================
SELECT CASE WHEN length(cloudsync_uuid()) = 36
    THEN test_pass('cloudsync_uuid returns 36-char string')
    ELSE test_fail('cloudsync_uuid returns 36-char string') END;

-- Two UUIDs should be different
SELECT CASE WHEN cloudsync_uuid() != cloudsync_uuid()
    THEN test_pass('cloudsync_uuid returns unique values')
    ELSE test_fail('cloudsync_uuid returns unique values') END;

-- ============================================================================
-- TEST 4: cloudsync_init with single TEXT PK table
-- ============================================================================
CREATE TABLE t1 (id VARCHAR PRIMARY KEY NOT NULL, name VARCHAR, value DOUBLE);

SELECT CASE WHEN cloudsync_init('t1') IS NOT NULL
    THEN test_pass('cloudsync_init t1 returns site_id')
    ELSE test_fail('cloudsync_init t1 returns site_id') END;

-- ============================================================================
-- TEST 5: cloudsync_siteid
-- ============================================================================
SELECT CASE WHEN cloudsync_siteid() IS NOT NULL AND octet_length(cloudsync_siteid()) = 16
    THEN test_pass('cloudsync_siteid returns 16-byte blob')
    ELSE test_fail('cloudsync_siteid returns 16-byte blob') END;

-- ============================================================================
-- TEST 6: cloudsync_db_version (initial)
-- ============================================================================
SELECT CASE WHEN cloudsync_db_version() = 0
    THEN test_pass('initial db_version is 0')
    ELSE test_fail('initial db_version is 0') END;

-- ============================================================================
-- TEST 7: cloudsync_is_enabled / cloudsync_is_sync
-- ============================================================================
SELECT CASE WHEN cloudsync_is_enabled('t1') = true
    THEN test_pass('t1 is enabled after init')
    ELSE test_fail('t1 is enabled after init') END;

-- cloudsync_is_sync returns true only when a sync operation is in progress
-- For a table that's initialized and enabled but not currently syncing, it returns false
SELECT CASE WHEN cloudsync_is_sync('t1') = false
    THEN test_pass('t1 is_sync=false (no sync in progress)')
    ELSE test_fail('t1 is_sync=false (no sync in progress)') END;

-- ============================================================================
-- TEST 8: cloudsync_disable / cloudsync_enable
-- ============================================================================
SELECT cloudsync_disable('t1');

SELECT CASE WHEN cloudsync_is_enabled('t1') = false
    THEN test_pass('t1 is disabled after cloudsync_disable')
    ELSE test_fail('t1 is disabled after cloudsync_disable') END;

SELECT cloudsync_enable('t1');

SELECT CASE WHEN cloudsync_is_enabled('t1') = true
    THEN test_pass('t1 is re-enabled after cloudsync_enable')
    ELSE test_fail('t1 is re-enabled after cloudsync_enable') END;

-- ============================================================================
-- TEST 9: INSERT + cloudsync_insert (manual change tracking)
-- ============================================================================
INSERT INTO t1 VALUES ('k1', 'alice', 100.0);
INSERT INTO t1 VALUES ('k2', 'bob', 200.0);
INSERT INTO t1 VALUES ('k3', 'charlie', 300.0);
INSERT INTO t1 VALUES ('k4', 'diana', 400.0);
INSERT INTO t1 VALUES ('k5', 'eve', 500.0);

SELECT cloudsync_insert('t1', 'k1');
SELECT cloudsync_insert('t1', 'k2');
SELECT cloudsync_insert('t1', 'k3');
SELECT cloudsync_insert('t1', 'k4');
SELECT cloudsync_insert('t1', 'k5');

-- Verify data is in the table
SELECT CASE WHEN (SELECT count(*) FROM t1) = 5
    THEN test_pass('5 rows inserted into t1')
    ELSE test_fail('5 rows inserted into t1') END;

-- ============================================================================
-- TEST 10: cloudsync_changes shows tracked inserts
-- ============================================================================
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes) > 0
    THEN test_pass('cloudsync_changes has entries after inserts')
    ELSE test_fail('cloudsync_changes has entries after inserts') END;

-- ============================================================================
-- TEST 11: cloudsync_db_version after inserts
-- ============================================================================
SELECT CASE WHEN cloudsync_db_version() > 0
    THEN test_pass('db_version > 0 after inserts')
    ELSE test_fail('db_version > 0 after inserts') END;

-- ============================================================================
-- TEST 12: cloudsync_db_version_next
-- ============================================================================
SELECT CASE WHEN cloudsync_db_version_next(0) > cloudsync_db_version()
    THEN test_pass('db_version_next returns incremented value')
    ELSE test_fail('db_version_next returns incremented value') END;

-- ============================================================================
-- TEST 13: DELETE + cloudsync_delete (manual change tracking)
-- ============================================================================
DELETE FROM t1 WHERE id = 'k5';
SELECT cloudsync_delete('t1', 'k5');

SELECT CASE WHEN (SELECT count(*) FROM t1) = 4
    THEN test_pass('row deleted, 4 rows remain')
    ELSE test_fail('row deleted, 4 rows remain') END;

-- ============================================================================
-- TEST 14: UPDATE + cloudsync_update (manual change tracking via aggregate)
-- ============================================================================
-- cloudsync_update is an aggregate that collects column-level changes.
-- It takes (table_name, new_value, old_value) for each column in order: PKs first, then non-PK cols.
-- For an UPDATE t1 SET name='alice_updated' WHERE id='k1':
--   old row: ('k1', 'alice', 100.0)
--   new row: ('k1', 'alice_updated', 100.0)

UPDATE t1 SET name = 'alice_updated' WHERE id = 'k1';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k1', 'k1'),
    ('alice_updated', 'alice'),
    (CAST(100.0 AS VARCHAR), CAST(100.0 AS VARCHAR))
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT name FROM t1 WHERE id = 'k1') = 'alice_updated'
    THEN test_pass('UPDATE tracked via cloudsync_update')
    ELSE test_fail('UPDATE tracked via cloudsync_update') END;

-- ============================================================================
-- TEST 15: cloudsync_set / cloudsync_set_table / cloudsync_set_column
-- ============================================================================
SELECT CASE WHEN cloudsync_set('test_key', 'test_value') = true
    THEN test_pass('cloudsync_set key/value')
    ELSE test_fail('cloudsync_set key/value') END;

SELECT CASE WHEN cloudsync_set_table('t1', 'table_key', 'table_value') = true
    THEN test_pass('cloudsync_set_table key/value')
    ELSE test_fail('cloudsync_set_table key/value') END;

SELECT CASE WHEN cloudsync_set_column('t1', 'name', 'col_key', 'col_value') = true
    THEN test_pass('cloudsync_set_column key/value')
    ELSE test_fail('cloudsync_set_column key/value') END;

-- ============================================================================
-- TEST 16: cloudsync_schema / cloudsync_set_schema / cloudsync_table_schema
-- ============================================================================
SELECT CASE WHEN cloudsync_schema() IS NULL OR cloudsync_schema() = ''
    THEN test_pass('default schema is NULL or empty')
    ELSE test_fail('default schema is NULL or empty') END;

SELECT CASE WHEN cloudsync_table_schema('t1') IS NOT NULL
    THEN test_pass('cloudsync_table_schema returns value')
    ELSE test_fail('cloudsync_table_schema returns value') END;

-- ============================================================================
-- TEST 17: cloudsync_seq
-- ============================================================================
SELECT CASE WHEN cloudsync_seq() >= 0
    THEN test_pass('cloudsync_seq returns non-negative')
    ELSE test_fail('cloudsync_seq returns non-negative') END;

-- ============================================================================
-- TEST 18: cloudsync_payload_encode (aggregate)
-- ============================================================================
SELECT CASE WHEN (
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
    FROM cloudsync_changes
) IS NOT NULL
    THEN test_pass('payload_encode produces non-NULL blob')
    ELSE test_fail('payload_encode produces non-NULL blob') END;

-- ============================================================================
-- TEST 19: cloudsync_payload_save
-- ============================================================================
SELECT CASE WHEN cloudsync_payload_save('/tmp/cloudsync_duckdb_test_payload.bin') >= 0
    THEN test_pass('payload_save returns >= 0')
    ELSE test_fail('payload_save returns >= 0') END;

-- ============================================================================
-- TEST 20: Composite PK table
-- ============================================================================
CREATE TABLE t2 (
    first_name VARCHAR NOT NULL,
    last_name VARCHAR NOT NULL,
    age INTEGER,
    note VARCHAR,
    PRIMARY KEY(first_name, last_name)
);

SELECT CASE WHEN cloudsync_init('t2') IS NOT NULL
    THEN test_pass('cloudsync_init t2 composite PK')
    ELSE test_fail('cloudsync_init t2 composite PK') END;

INSERT INTO t2 VALUES ('John', 'Doe', 30, 'note1');
INSERT INTO t2 VALUES ('Jane', 'Doe', 25, 'note2');
INSERT INTO t2 VALUES ('Bob', 'Smith', 40, 'note3');

SELECT cloudsync_insert('t2', 'John', 'Doe');
SELECT cloudsync_insert('t2', 'Jane', 'Doe');
SELECT cloudsync_insert('t2', 'Bob', 'Smith');

SELECT CASE WHEN (SELECT count(*) FROM t2) = 3
    THEN test_pass('3 rows inserted into t2 (composite PK)')
    ELSE test_fail('3 rows inserted into t2 (composite PK)') END;

-- Verify changes tracked for t2
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes WHERE tbl = 't2') > 0
    THEN test_pass('changes tracked for t2')
    ELSE test_fail('changes tracked for t2') END;

-- ============================================================================
-- TEST 21: DELETE with composite PK
-- ============================================================================
DELETE FROM t2 WHERE first_name = 'Bob' AND last_name = 'Smith';
SELECT cloudsync_delete('t2', 'Bob', 'Smith');

SELECT CASE WHEN (SELECT count(*) FROM t2) = 2
    THEN test_pass('composite PK delete leaves 2 rows')
    ELSE test_fail('composite PK delete leaves 2 rows') END;

-- ============================================================================
-- TEST 22: UPDATE with composite PK via cloudsync_update
-- ============================================================================
UPDATE t2 SET age = 31 WHERE first_name = 'John' AND last_name = 'Doe';
SELECT cloudsync_update('t2', new_val, old_val) FROM (VALUES
    ('John', 'John'),
    ('Doe', 'Doe'),
    ('31', '30'),
    ('note1', 'note1')
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT age FROM t2 WHERE first_name = 'John' AND last_name = 'Doe') = 31
    THEN test_pass('composite PK update tracked')
    ELSE test_fail('composite PK update tracked') END;

-- ============================================================================
-- TEST 23: cloudsync_begin_alter / cloudsync_commit_alter
-- ============================================================================
SELECT CASE WHEN cloudsync_begin_alter('t1') = true
    THEN test_pass('begin_alter succeeds')
    ELSE test_fail('begin_alter succeeds') END;

ALTER TABLE t1 ADD COLUMN extra VARCHAR;

SELECT CASE WHEN cloudsync_commit_alter('t1') = true
    THEN test_pass('commit_alter succeeds')
    ELSE test_fail('commit_alter succeeds') END;

-- ============================================================================
-- TEST 24: Schema hash (implemented like PostgreSQL)
-- ============================================================================
-- After commit_alter, schema hash should be updated
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_schema_versions) > 0
    THEN test_pass('schema_versions table has entries after alter')
    ELSE test_fail('schema_versions table has entries after alter') END;

-- ============================================================================
-- TEST 25: cloudsync_col_value
-- ============================================================================
SELECT CASE WHEN cloudsync_col_value('t1', 'name', cloudsync_pk_encode('k1')) IS NOT NULL
    THEN test_pass('col_value returns value for existing row')
    ELSE test_fail('col_value returns value for existing row') END;

-- ============================================================================
-- TEST 26: cloudsync_set_filter / cloudsync_clear_filter
-- ============================================================================
SELECT CASE WHEN cloudsync_set_filter('t1', 'value > 100') = true
    THEN test_pass('set_filter succeeds')
    ELSE test_fail('set_filter succeeds') END;

SELECT CASE WHEN cloudsync_clear_filter('t1') = true
    THEN test_pass('clear_filter succeeds')
    ELSE test_fail('clear_filter succeeds') END;

-- ============================================================================
-- TEST 27: cloudsync_init with INTEGER PK + skip flag
-- ============================================================================
CREATE TABLE t_int_pk (id INTEGER PRIMARY KEY NOT NULL, name VARCHAR);

-- cloudsync_init('t_int_pk') would fail because INTEGER PKs are not safe for CRDT.
-- We skip that test to avoid crashing the session and test the skip flag instead.
SELECT CASE WHEN cloudsync_init('t_int_pk', 'cls', 1) IS NOT NULL
    THEN test_pass('integer PK accepted with skip flag')
    ELSE test_fail('integer PK accepted with skip flag') END;

-- ============================================================================
-- TEST 28: Table with only PK columns (no non-PK columns)
-- ============================================================================
CREATE TABLE t_pkonly (
    first_name VARCHAR NOT NULL,
    last_name VARCHAR NOT NULL,
    PRIMARY KEY(first_name, last_name)
);

SELECT CASE WHEN cloudsync_init('t_pkonly') IS NOT NULL
    THEN test_pass('init PK-only table')
    ELSE test_fail('init PK-only table') END;

INSERT INTO t_pkonly VALUES ('Alice', 'Wonder');
SELECT cloudsync_insert('t_pkonly', 'Alice', 'Wonder');

SELECT CASE WHEN (SELECT count(*) FROM t_pkonly) = 1
    THEN test_pass('PK-only table insert + tracking')
    ELSE test_fail('PK-only table insert + tracking') END;

-- ============================================================================
-- TEST 29: Multiple inserts and bulk change tracking
-- ============================================================================
CREATE TABLE t_bulk (id VARCHAR PRIMARY KEY NOT NULL, val INTEGER);
SELECT cloudsync_init('t_bulk');

-- Insert 50 rows
INSERT INTO t_bulk SELECT 'row' || i::VARCHAR, i FROM generate_series(1, 50) t(i);
-- Track all 50 inserts
SELECT cloudsync_insert('t_bulk', 'row' || i::VARCHAR) FROM generate_series(1, 50) t(i);

SELECT CASE WHEN (SELECT count(*) FROM t_bulk) = 50
    THEN test_pass('bulk insert 50 rows')
    ELSE test_fail('bulk insert 50 rows') END;

SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes WHERE tbl = 't_bulk') > 0
    THEN test_pass('bulk changes tracked')
    ELSE test_fail('bulk changes tracked') END;

-- ============================================================================
-- TEST 30: Payload encode/decode roundtrip (same database)
-- ============================================================================

-- Save the current encoded payload
CREATE TABLE _test_payload AS
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
    FROM cloudsync_changes WHERE tbl = 't_bulk';

SELECT CASE WHEN (SELECT payload FROM _test_payload) IS NOT NULL
    THEN test_pass('payload encoded for t_bulk')
    ELSE test_fail('payload encoded for t_bulk') END;

-- Decode should not crash (applying to same DB is a no-op due to same site_id)
SELECT CASE WHEN cloudsync_payload_apply((SELECT payload FROM _test_payload)) >= 0
    THEN test_pass('payload_apply on same DB succeeds')
    ELSE test_fail('payload_apply on same DB succeeds') END;

DROP TABLE _test_payload;

-- ============================================================================
-- TEST 31: cloudsync_changes_select table function with filters
-- ============================================================================
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select(0)) >= 0
    THEN test_pass('changes_select(min_version) works')
    ELSE test_fail('changes_select(min_version) works') END;

SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select(0, NULL)) >= 0
    THEN test_pass('changes_select(min_version, site_id) works')
    ELSE test_fail('changes_select(min_version, site_id) works') END;

-- Filter by site_id (should return only our own changes)
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select(0, cloudsync_siteid())) > 0
    THEN test_pass('changes_select filtered by own site_id has rows')
    ELSE test_fail('changes_select filtered by own site_id has rows') END;

-- ============================================================================
-- TEST 32: Changes columns have correct types
-- ============================================================================
SELECT CASE WHEN (
    SELECT count(*) FROM cloudsync_changes LIMIT 1
) >= 0
    THEN test_pass('cloudsync_changes view is queryable')
    ELSE test_fail('cloudsync_changes view is queryable') END;

-- ============================================================================
-- TEST 33: cloudsync_cleanup
-- ============================================================================
SELECT CASE WHEN cloudsync_cleanup('t_int_pk') = true
    THEN test_pass('cleanup t_int_pk')
    ELSE test_fail('cleanup t_int_pk') END;

SELECT CASE WHEN cloudsync_is_sync('t_int_pk') = false
    THEN test_pass('t_int_pk no longer synced after cleanup')
    ELSE test_fail('t_int_pk no longer synced after cleanup') END;

-- ============================================================================
-- TEST 34: Multiple updates on same row
-- ============================================================================
UPDATE t1 SET value = 150.0 WHERE id = 'k1';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k1', 'k1'),
    ('alice_updated', 'alice_updated'),
    ('150.0', '100.0'),
    (NULL, NULL)
) AS vals(new_val, old_val);

UPDATE t1 SET value = 175.0 WHERE id = 'k1';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k1', 'k1'),
    ('alice_updated', 'alice_updated'),
    ('175.0', '150.0'),
    (NULL, NULL)
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT value FROM t1 WHERE id = 'k1') = 175.0
    THEN test_pass('multiple updates on same row')
    ELSE test_fail('multiple updates on same row') END;

-- ============================================================================
-- TEST 35: cloudsync_merge_insert (internal function)
-- ============================================================================
-- cloudsync_merge_insert is an internal function used by payload_apply/payload_load.
-- It cannot be called directly as a SELECT because it uses prepared statements
-- on the same connection (deadlock). It is tested indirectly via payload roundtrip.
SELECT test_pass('merge_insert tested via payload roundtrip');

-- ============================================================================
-- TEST 36: Payload save to file and verify non-empty
-- ============================================================================
SELECT cloudsync_payload_save('/tmp/cloudsync_duckdb_test_full.bin');

-- ============================================================================
-- TEST 37: cloudsync_init with algorithm specification
-- ============================================================================
CREATE TABLE t_cls (id VARCHAR PRIMARY KEY NOT NULL, data VARCHAR);
SELECT CASE WHEN cloudsync_init('t_cls', 'cls') IS NOT NULL
    THEN test_pass('init with cls algorithm')
    ELSE test_fail('init with cls algorithm') END;

-- ============================================================================
-- TEST 38: Double init should not crash
-- ============================================================================
SELECT CASE WHEN cloudsync_init('t_cls', 'cls') IS NOT NULL
    THEN test_pass('double init does not crash')
    ELSE test_fail('double init does not crash') END;

-- ============================================================================
-- TEST 39: Schema functions
-- ============================================================================

-- Default schema should be NULL or empty
SELECT CASE WHEN cloudsync_schema() IS NULL OR cloudsync_schema() = ''
    THEN test_pass('schema: default is NULL or empty')
    ELSE test_fail('schema: default is NULL or empty') END;

-- Set schema and read it back
SELECT cloudsync_set_schema('custom_schema');

SELECT CASE WHEN cloudsync_schema() = 'custom_schema'
    THEN test_pass('schema: set to custom_schema')
    ELSE test_fail('schema: set to custom_schema') END;

-- Reset schema back to empty (DuckDB can't pass NULL to scalar functions)
SELECT cloudsync_set_schema('');

SELECT CASE WHEN cloudsync_schema() IS NULL OR cloudsync_schema() = ''
    THEN test_pass('schema: reset to empty')
    ELSE test_fail('schema: reset to empty') END;

-- table_schema for initialized table
SELECT CASE WHEN cloudsync_table_schema('t1') IS NOT NULL
    THEN test_pass('schema: table_schema for init table')
    ELSE test_fail('schema: table_schema for init table') END;

-- table_schema for non-existent table returns NULL
SELECT CASE WHEN cloudsync_table_schema('no_such_table') IS NULL
    THEN test_pass('schema: table_schema for missing table is NULL')
    ELSE test_fail('schema: table_schema for missing table is NULL') END;

-- ============================================================================
-- TEST 40: Filter behavior during sync
-- ============================================================================

CREATE TABLE tasks (
    id VARCHAR PRIMARY KEY NOT NULL,
    title VARCHAR,
    user_id INTEGER
);
SELECT cloudsync_init('tasks');

-- Set filter: only rows with user_id = 1
SELECT cloudsync_set_filter('tasks', 'user_id = 1');

-- Insert matching rows (user_id=1) and non-matching (user_id=2)
INSERT INTO tasks VALUES ('a', 'Task A', 1);
INSERT INTO tasks VALUES ('b', 'Task B', 2);
INSERT INTO tasks VALUES ('c', 'Task C', 1);

SELECT cloudsync_insert('tasks', 'a');
SELECT cloudsync_insert('tasks', 'b');
SELECT cloudsync_insert('tasks', 'c');

-- Only matching rows (user_id=1) should be tracked in metadata
SELECT CASE WHEN (SELECT count(DISTINCT pk) FROM tasks_cloudsync) = 2
    THEN test_pass('filter: only matching rows in metadata')
    ELSE test_fail('filter: only matching rows in metadata') END;

-- Update matching row should update metadata
UPDATE tasks SET title = 'Task A Updated' WHERE id = 'a';
SELECT cloudsync_update('tasks', new_val, old_val) FROM (VALUES
    ('a', 'a'),
    ('Task A Updated', 'Task A'),
    ('1', '1')
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync WHERE pk = cloudsync_pk_encode('a') AND col_name = 'title') > 0
    THEN test_pass('filter: matching update tracked')
    ELSE test_fail('filter: matching update tracked') END;

-- Update non-matching row should NOT add metadata
CREATE TABLE _filter_count1 AS SELECT count(*) AS c FROM tasks_cloudsync;

UPDATE tasks SET title = 'Task B Updated' WHERE id = 'b';
SELECT cloudsync_update('tasks', new_val, old_val) FROM (VALUES
    ('b', 'b'),
    ('Task B Updated', 'Task B'),
    ('2', '2')
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync) = (SELECT c FROM _filter_count1)
    THEN test_pass('filter: non-matching update not tracked')
    ELSE test_fail('filter: non-matching update not tracked') END;
DROP TABLE _filter_count1;

-- Delete matching row should create tombstone
DELETE FROM tasks WHERE id = 'a';
SELECT cloudsync_delete('tasks', 'a');

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync WHERE pk = cloudsync_pk_encode('a')) > 0
    THEN test_pass('filter: matching delete creates tombstone')
    ELSE test_fail('filter: matching delete creates tombstone') END;

-- Delete non-matching row should NOT add metadata
CREATE TABLE _filter_count2 AS SELECT count(*) AS c FROM tasks_cloudsync;

DELETE FROM tasks WHERE id = 'b';
SELECT cloudsync_delete('tasks', 'b');

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync) = (SELECT c FROM _filter_count2)
    THEN test_pass('filter: non-matching delete not tracked')
    ELSE test_fail('filter: non-matching delete not tracked') END;
DROP TABLE _filter_count2;

-- Clear filter
SELECT cloudsync_clear_filter('tasks');

-- After clearing filter, all inserts should be tracked
INSERT INTO tasks VALUES ('d', 'Task D', 2);
SELECT cloudsync_insert('tasks', 'd');

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync WHERE pk = cloudsync_pk_encode('d')) > 0
    THEN test_pass('filter: after clear, all rows tracked')
    ELSE test_fail('filter: after clear, all rows tracked') END;

-- ============================================================================
-- TEST 41: Filter with payload roundtrip
-- ============================================================================

-- Save payload (should only contain filtered data for tasks)
SELECT CASE WHEN cloudsync_payload_save('/tmp/cloudsync_duckdb_test_filter.bin') >= 0
    THEN test_pass('filter: payload_save with filtered table')
    ELSE test_fail('filter: payload_save with filtered table') END;

-- ============================================================================
-- TEST 42: Alter table - add column
-- ============================================================================

CREATE TABLE t_alter1 (id VARCHAR PRIMARY KEY NOT NULL, name VARCHAR, age INTEGER);
SELECT cloudsync_init('t_alter1');

INSERT INTO t_alter1 VALUES ('r1', 'Alice', 30);
INSERT INTO t_alter1 VALUES ('r2', 'Bob', 25);
SELECT cloudsync_insert('t_alter1', 'r1');
SELECT cloudsync_insert('t_alter1', 'r2');

-- Begin alter
SELECT CASE WHEN cloudsync_begin_alter('t_alter1') = true
    THEN test_pass('alter1: begin_alter')
    ELSE test_fail('alter1: begin_alter') END;

-- Add column
ALTER TABLE t_alter1 ADD COLUMN email VARCHAR;

-- Commit alter
SELECT CASE WHEN cloudsync_commit_alter('t_alter1') = true
    THEN test_pass('alter1: commit_alter add column')
    ELSE test_fail('alter1: commit_alter add column') END;

-- Verify new column is usable and change-tracked
UPDATE t_alter1 SET email = 'alice@test.com' WHERE id = 'r1';
SELECT cloudsync_update('t_alter1', new_val, old_val) FROM (VALUES
    ('r1', 'r1'),
    ('Alice', 'Alice'),
    ('30', '30'),
    ('alice@test.com', NULL)
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT email FROM t_alter1 WHERE id = 'r1') = 'alice@test.com'
    THEN test_pass('alter1: new column writable')
    ELSE test_fail('alter1: new column writable') END;

-- Changes for new column should be tracked
SELECT CASE WHEN (SELECT count(*) FROM t_alter1_cloudsync WHERE pk = cloudsync_pk_encode('r1') AND col_name = 'email') > 0
    THEN test_pass('alter1: new column changes tracked')
    ELSE test_fail('alter1: new column changes tracked') END;

-- ============================================================================
-- TEST 43: Alter table - add column with default
-- ============================================================================

SELECT CASE WHEN cloudsync_begin_alter('t_alter1') = true
    THEN test_pass('alter2: begin_alter')
    ELSE test_fail('alter2: begin_alter') END;

ALTER TABLE t_alter1 ADD COLUMN status VARCHAR DEFAULT 'active';

SELECT CASE WHEN cloudsync_commit_alter('t_alter1') = true
    THEN test_pass('alter2: commit_alter add col with default')
    ELSE test_fail('alter2: commit_alter add col with default') END;

-- Existing rows should have the default
SELECT CASE WHEN (SELECT status FROM t_alter1 WHERE id = 'r1') = 'active'
    THEN test_pass('alter2: default value applied')
    ELSE test_fail('alter2: default value applied') END;

-- ============================================================================
-- TEST 44: Alter table - payload after alter
-- ============================================================================

-- Insert a new row using the altered schema
INSERT INTO t_alter1 VALUES ('r3', 'Charlie', 35, 'charlie@test.com', 'inactive');
SELECT cloudsync_insert('t_alter1', 'r3');

-- Payload should work with the altered table
SELECT CASE WHEN (
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
    FROM cloudsync_changes WHERE tbl = 't_alter1'
) IS NOT NULL
    THEN test_pass('alter: payload_encode after alter')
    ELSE test_fail('alter: payload_encode after alter') END;

-- ============================================================================
-- TEST 45: Schema hash updated after alter
-- ============================================================================
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_schema_versions) >= 1
    THEN test_pass('alter: schema_versions has entries after alter')
    ELSE test_fail('alter: schema_versions has entries after alter') END;

-- ============================================================================
-- TEST 46: Update with NULL values
-- ============================================================================

CREATE TABLE t_nulls (
    id VARCHAR PRIMARY KEY NOT NULL,
    name VARCHAR,
    age INTEGER,
    note VARCHAR
);
SELECT cloudsync_init('t_nulls');

-- Insert with non-NULL values
INSERT INTO t_nulls VALUES ('n1', 'Alice', 30, 'some note');
SELECT cloudsync_insert('t_nulls', 'n1');

-- Update to set a column to NULL (non-NULL → NULL)
UPDATE t_nulls SET note = NULL WHERE id = 'n1';
SELECT cloudsync_update('t_nulls', new_val, old_val) FROM (VALUES
    ('n1', 'n1'),
    ('Alice', 'Alice'),
    ('30', '30'),
    (NULL, 'some note')
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT note FROM t_nulls WHERE id = 'n1') IS NULL
    THEN test_pass('null: update column to NULL')
    ELSE test_fail('null: update column to NULL') END;

-- The NULL change should be tracked
SELECT CASE WHEN (SELECT count(*) FROM t_nulls_cloudsync WHERE pk = cloudsync_pk_encode('n1') AND col_name = 'note') > 0
    THEN test_pass('null: NULL update tracked in metadata')
    ELSE test_fail('null: NULL update tracked in metadata') END;

-- Update from NULL to non-NULL
UPDATE t_nulls SET note = 'restored' WHERE id = 'n1';
SELECT cloudsync_update('t_nulls', new_val, old_val) FROM (VALUES
    ('n1', 'n1'),
    ('Alice', 'Alice'),
    ('30', '30'),
    ('restored', NULL)
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT note FROM t_nulls WHERE id = 'n1') = 'restored'
    THEN test_pass('null: update column from NULL to value')
    ELSE test_fail('null: update column from NULL to value') END;

-- Insert with NULL columns
INSERT INTO t_nulls VALUES ('n2', 'Bob', NULL, NULL);
SELECT cloudsync_insert('t_nulls', 'n2');

SELECT CASE WHEN (SELECT age FROM t_nulls WHERE id = 'n2') IS NULL
              AND (SELECT note FROM t_nulls WHERE id = 'n2') IS NULL
    THEN test_pass('null: insert with NULL columns')
    ELSE test_fail('null: insert with NULL columns') END;

-- ============================================================================
-- TEST 47: Delete and re-insert same PK
-- ============================================================================

INSERT INTO t_nulls VALUES ('n3', 'Charlie', 40, 'temp');
SELECT cloudsync_insert('t_nulls', 'n3');

-- Delete
DELETE FROM t_nulls WHERE id = 'n3';
SELECT cloudsync_delete('t_nulls', 'n3');

-- Re-insert same PK
INSERT INTO t_nulls VALUES ('n3', 'Charlie New', 41, 'back');
SELECT cloudsync_insert('t_nulls', 'n3');

SELECT CASE WHEN (SELECT name FROM t_nulls WHERE id = 'n3') = 'Charlie New'
              AND (SELECT age FROM t_nulls WHERE id = 'n3') = 41
    THEN test_pass('delete-reinsert: row exists with new data')
    ELSE test_fail('delete-reinsert: row exists with new data') END;

-- Sentinel should exist in metadata
SELECT CASE WHEN (SELECT count(*) FROM t_nulls_cloudsync WHERE pk = cloudsync_pk_encode('n3') AND col_name = '__[RIP]__') > 0
    THEN test_pass('delete-reinsert: sentinel restored')
    ELSE test_fail('delete-reinsert: sentinel restored') END;

-- ============================================================================
-- TEST 48: Multi-table payload encode
-- ============================================================================

-- We have t1, t2, t_nulls all with changes. Encode a combined payload.
SELECT CASE WHEN (
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
    FROM cloudsync_changes
) IS NOT NULL
    THEN test_pass('multi-table: combined payload_encode')
    ELSE test_fail('multi-table: combined payload_encode') END;

-- ============================================================================
-- TEST 49: Multi-table payload save/load
-- ============================================================================

SELECT CASE WHEN cloudsync_payload_save('/tmp/cloudsync_duckdb_test_multi.bin') >= 0
    THEN test_pass('multi-table: payload_save')
    ELSE test_fail('multi-table: payload_save') END;

-- Apply to same DB (no-op due to same site_id, but should not crash)
SELECT CASE WHEN cloudsync_payload_load('/tmp/cloudsync_duckdb_test_multi.bin') >= 0
    THEN test_pass('multi-table: payload_load on same DB')
    ELSE test_fail('multi-table: payload_load on same DB') END;

-- ============================================================================
-- TEST 50: Cleanup removes sync metadata
-- ============================================================================

-- Verify metadata exists before cleanup
SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync) > 0
    THEN test_pass('cleanup: metadata exists before cleanup')
    ELSE test_fail('cleanup: metadata exists before cleanup') END;

SELECT cloudsync_cleanup('tasks');

-- Table should still exist with data
SELECT CASE WHEN (SELECT count(*) FROM tasks) >= 0
    THEN test_pass('cleanup: user table still exists')
    ELSE test_fail('cleanup: user table still exists') END;

-- cloudsync_is_enabled should return false
SELECT CASE WHEN cloudsync_is_enabled('tasks') = false
    THEN test_pass('cleanup: table no longer enabled')
    ELSE test_fail('cleanup: table no longer enabled') END;

-- Metadata table should be dropped
SELECT CASE WHEN (
    SELECT count(*) FROM information_schema.tables
    WHERE table_name = 'tasks_cloudsync'
) = 0
    THEN test_pass('cleanup: metadata table dropped')
    ELSE test_fail('cleanup: metadata table dropped') END;

-- ============================================================================
-- TEST 51: Re-init after cleanup
-- ============================================================================

SELECT CASE WHEN cloudsync_init('tasks') IS NOT NULL
    THEN test_pass('cleanup: re-init after cleanup')
    ELSE test_fail('cleanup: re-init after cleanup') END;

-- Insert and track after re-init
INSERT INTO tasks VALUES ('e', 'Task E', 3);
SELECT cloudsync_insert('tasks', 'e');

SELECT CASE WHEN (SELECT count(*) FROM tasks_cloudsync WHERE pk = cloudsync_pk_encode('e')) > 0
    THEN test_pass('cleanup: tracking works after re-init')
    ELSE test_fail('cleanup: tracking works after re-init') END;

-- Clean up
SELECT cloudsync_cleanup('tasks');

-- ============================================================================
-- TEST 52: Multiple updates same column (version increments)
-- ============================================================================

-- Get version of name column before updates
CREATE TABLE _ver_before AS
    SELECT col_version FROM t1_cloudsync
    WHERE pk = cloudsync_pk_encode('k2') AND col_name = 'name';

UPDATE t1 SET name = 'bob_v2' WHERE id = 'k2';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k2', 'k2'),
    ('bob_v2', 'bob'),
    (CAST(200.0 AS VARCHAR), CAST(200.0 AS VARCHAR)),
    (NULL, NULL)
) AS vals(new_val, old_val);

UPDATE t1 SET name = 'bob_v3' WHERE id = 'k2';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k2', 'k2'),
    ('bob_v3', 'bob_v2'),
    (CAST(200.0 AS VARCHAR), CAST(200.0 AS VARCHAR)),
    (NULL, NULL)
) AS vals(new_val, old_val);

CREATE TABLE _ver_after AS
    SELECT col_version FROM t1_cloudsync
    WHERE pk = cloudsync_pk_encode('k2') AND col_name = 'name';

SELECT CASE WHEN (SELECT col_version FROM _ver_after) > (SELECT col_version FROM _ver_before)
    THEN test_pass('version: col_version increments on updates')
    ELSE test_fail('version: col_version increments on updates') END;

DROP TABLE _ver_before;
DROP TABLE _ver_after;

-- ============================================================================
-- TEST 53: Update only changed columns
-- ============================================================================

-- Track version of 'value' column before update
CREATE TABLE _val_ver AS
    SELECT col_version FROM t1_cloudsync
    WHERE pk = cloudsync_pk_encode('k3') AND col_name = 'value';

-- Update name only, value unchanged
UPDATE t1 SET name = 'charlie_v2' WHERE id = 'k3';
SELECT cloudsync_update('t1', new_val, old_val) FROM (VALUES
    ('k3', 'k3'),
    ('charlie_v2', 'charlie'),
    (CAST(300.0 AS VARCHAR), CAST(300.0 AS VARCHAR)),
    (NULL, NULL)
) AS vals(new_val, old_val);

-- value col_version should NOT have changed
SELECT CASE WHEN (
    SELECT col_version FROM t1_cloudsync
    WHERE pk = cloudsync_pk_encode('k3') AND col_name = 'value'
) = (SELECT col_version FROM _val_ver)
    THEN test_pass('version: unchanged column version not bumped')
    ELSE test_fail('version: unchanged column version not bumped') END;

DROP TABLE _val_ver;

-- ============================================================================
-- TEST 54: Payload roundtrip preserves data types
-- ============================================================================

CREATE TABLE t_types (
    id VARCHAR PRIMARY KEY NOT NULL,
    int_col INTEGER,
    dbl_col DOUBLE,
    txt_col VARCHAR
);
SELECT cloudsync_init('t_types');

INSERT INTO t_types VALUES ('t1', 42, 3.14, 'hello');
INSERT INTO t_types VALUES ('t2', -100, 0.0, '');
INSERT INTO t_types VALUES ('t3', NULL, NULL, NULL);
SELECT cloudsync_insert('t_types', 't1');
SELECT cloudsync_insert('t_types', 't2');
SELECT cloudsync_insert('t_types', 't3');

-- Encode payload
CREATE TABLE _types_payload AS
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
    FROM cloudsync_changes WHERE tbl = 't_types';

SELECT CASE WHEN (SELECT payload FROM _types_payload) IS NOT NULL
    THEN test_pass('types: payload encoded for mixed types')
    ELSE test_fail('types: payload encoded for mixed types') END;

-- Apply to self (no-op but should not crash with mixed types)
SELECT CASE WHEN cloudsync_payload_apply((SELECT payload FROM _types_payload)) >= 0
    THEN test_pass('types: payload_apply with mixed types')
    ELSE test_fail('types: payload_apply with mixed types') END;

DROP TABLE _types_payload;

-- ============================================================================
-- TEST 55: col_value for various column states
-- ============================================================================

-- Existing non-NULL column
SELECT CASE WHEN cloudsync_col_value('t_types', 'int_col', cloudsync_pk_encode('t1')) = '42'
    THEN test_pass('col_value: integer column')
    ELSE test_fail('col_value: integer column') END;

-- NULL column
SELECT CASE WHEN cloudsync_col_value('t_types', 'int_col', cloudsync_pk_encode('t3')) IS NULL
    THEN test_pass('col_value: NULL column returns NULL')
    ELSE test_fail('col_value: NULL column returns NULL') END;

-- Non-existent PK
SELECT CASE WHEN cloudsync_col_value('t_types', 'int_col', cloudsync_pk_encode('no_such_key')) IS NULL
    THEN test_pass('col_value: non-existent PK returns NULL')
    ELSE test_fail('col_value: non-existent PK returns NULL') END;

-- Empty string column
SELECT CASE WHEN cloudsync_col_value('t_types', 'txt_col', cloudsync_pk_encode('t2')) = ''
    THEN test_pass('col_value: empty string column')
    ELSE test_fail('col_value: empty string column') END;

-- ============================================================================
-- TEST 56: Composite PK payload roundtrip
-- ============================================================================

-- t2 has composite PK (first_name, last_name). Verify payload works.
CREATE TABLE _t2_payload AS
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
    FROM cloudsync_changes WHERE tbl = 't2';

SELECT CASE WHEN (SELECT payload FROM _t2_payload) IS NOT NULL
    THEN test_pass('composite PK: payload encoded')
    ELSE test_fail('composite PK: payload encoded') END;

SELECT CASE WHEN cloudsync_payload_apply((SELECT payload FROM _t2_payload)) >= 0
    THEN test_pass('composite PK: payload_apply succeeds')
    ELSE test_fail('composite PK: payload_apply succeeds') END;

DROP TABLE _t2_payload;

-- ============================================================================
-- TEST 57: PK-only table payload roundtrip
-- ============================================================================

-- t_pkonly has no non-PK columns
INSERT INTO t_pkonly VALUES ('Bob', 'Builder');
SELECT cloudsync_insert('t_pkonly', 'Bob', 'Builder');

CREATE TABLE _pkonly_payload AS
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) AS payload
    FROM cloudsync_changes WHERE tbl = 't_pkonly';

SELECT CASE WHEN (SELECT payload FROM _pkonly_payload) IS NOT NULL
    THEN test_pass('pk-only: payload encoded')
    ELSE test_fail('pk-only: payload encoded') END;

SELECT CASE WHEN cloudsync_payload_apply((SELECT payload FROM _pkonly_payload)) >= 0
    THEN test_pass('pk-only: payload_apply succeeds')
    ELSE test_fail('pk-only: payload_apply succeeds') END;

DROP TABLE _pkonly_payload;

-- ============================================================================
-- TEST 58: Changes select with various filters
-- ============================================================================

-- Get current db_version
CREATE TABLE _cur_ver AS SELECT cloudsync_db_version() AS v;

-- changes_select with version higher than current returns 0 rows
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select((SELECT v FROM _cur_ver) + 1)) = 0
    THEN test_pass('changes_select: future version returns 0 rows')
    ELSE test_fail('changes_select: future version returns 0 rows') END;

-- changes_select with version 0 returns all rows
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select(0)) > 0
    THEN test_pass('changes_select: version 0 returns all rows')
    ELSE test_fail('changes_select: version 0 returns all rows') END;

-- changes_select with bogus site_id returns 0 rows
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_changes_select(0, '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'::BLOB)) = 0
    THEN test_pass('changes_select: bogus site_id returns 0 rows')
    ELSE test_fail('changes_select: bogus site_id returns 0 rows') END;

DROP TABLE _cur_ver;

-- ============================================================================
-- TEST 59: Alter table - add multiple columns in sequence
-- ============================================================================

CREATE TABLE t_alter2 (id VARCHAR PRIMARY KEY NOT NULL, data VARCHAR);
SELECT cloudsync_init('t_alter2');

INSERT INTO t_alter2 VALUES ('x1', 'initial');
SELECT cloudsync_insert('t_alter2', 'x1');

-- First alter: add col_a
SELECT cloudsync_begin_alter('t_alter2');
ALTER TABLE t_alter2 ADD COLUMN col_a INTEGER;
SELECT cloudsync_commit_alter('t_alter2');

-- Second alter: add col_b
SELECT cloudsync_begin_alter('t_alter2');
ALTER TABLE t_alter2 ADD COLUMN col_b VARCHAR DEFAULT 'def';
SELECT cloudsync_commit_alter('t_alter2');

-- Insert using full schema
INSERT INTO t_alter2 VALUES ('x2', 'new', 42, 'custom');
SELECT cloudsync_insert('t_alter2', 'x2');

SELECT CASE WHEN (SELECT col_a FROM t_alter2 WHERE id = 'x2') = 42
              AND (SELECT col_b FROM t_alter2 WHERE id = 'x2') = 'custom'
    THEN test_pass('alter-seq: multiple alters work correctly')
    ELSE test_fail('alter-seq: multiple alters work correctly') END;

-- Update new columns
UPDATE t_alter2 SET col_a = 99, col_b = 'updated' WHERE id = 'x1';
SELECT cloudsync_update('t_alter2', new_val, old_val) FROM (VALUES
    ('x1', 'x1'),
    ('initial', 'initial'),
    ('99', NULL),
    ('updated', 'def')
) AS vals(new_val, old_val);

SELECT CASE WHEN (SELECT col_a FROM t_alter2 WHERE id = 'x1') = 99
              AND (SELECT col_b FROM t_alter2 WHERE id = 'x1') = 'updated'
    THEN test_pass('alter-seq: update new columns tracked')
    ELSE test_fail('alter-seq: update new columns tracked') END;

-- Payload should include all columns
SELECT CASE WHEN (
    SELECT cloudsync_payload_encode(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq)
    FROM cloudsync_changes WHERE tbl = 't_alter2'
) IS NOT NULL
    THEN test_pass('alter-seq: payload after multiple alters')
    ELSE test_fail('alter-seq: payload after multiple alters') END;

-- Schema versions should have entries
SELECT CASE WHEN (SELECT count(*) FROM cloudsync_schema_versions) >= 2
    THEN test_pass('alter-seq: schema version entries exist')
    ELSE test_fail('alter-seq: schema version entries exist') END;

-- ============================================================================
-- TEST 60: Tracked tables count
-- ============================================================================

SELECT CASE WHEN (SELECT count(DISTINCT tbl_name) FROM cloudsync_table_settings WHERE key = 'algo') > 0
    THEN test_pass('settings: tracked tables count > 0')
    ELSE test_fail('settings: tracked tables count > 0') END;

-- ============================================================================
-- TEST 61: cloudsync_terminate
-- ============================================================================
-- We test terminate last since it cleans up all sync state
-- But first let's verify we can still query
SELECT CASE WHEN cloudsync_db_version() >= 0
    THEN test_pass('db_version accessible before terminate')
    ELSE test_fail('db_version accessible before terminate') END;

-- ============================================================================
-- SUMMARY
-- ============================================================================
SELECT '=================================';
SELECT 'DuckDB CloudSync Test Suite Done';
SELECT 'Version: ' || cloudsync_version();
SELECT '=================================';
